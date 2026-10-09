/**
 * @file    esp_prov.c
 * @brief   WiFi 配网模式实现（见 esp_prov.h 的说明）
 *
 * ---------------------------------------------------------------------------
 * 一句话原理
 * ---------------------------------------------------------------------------
 *   ESP8266 切成 AP+STA，开一个软热点和一个 TCP 服务器（80 端口），
 *   在服务器上跑一个"极简 HTTP"：GET / 给网页、GET /scan 返回附近 WiFi 列表、
 *   POST /save 收下 SSID/密码写进 Flash。全程只用 AT 指令，平台无关。
 *
 * 为什么不用现成的分帧解析？
 *   服务器模式（AT+CIPMUX=1）里收到的请求帧是 "+IPD,<id>,<len>:"，比普通分帧
 *   多一个 link id 字段，驱动的分流器不认。于是进配网前 esp_at_set_raw_rx(1)
 *   把分流关掉，字节全进 AT 文本环，本文件的 prov_feed_byte() 自己解析。
 *
 * 首发协议栈：MIT
 */

#include "esp_prov.h"

#if ESP_PROV_ENABLE

#include "esp8266_at.h"
#include "esp_net_config.h"
#include "esp_cred.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------- 迷你网页 */
/* 放 Flash 的 const，不占 RAM。用原生 JS fetch('/scan') 拉列表再填下拉框。 */
static const char kIndexHtml[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>STM32 WiFi Setup</title>"
"<style>body{font-family:sans-serif;margin:20px;max-width:440px}"
"h1{font-size:20px}select,input,button{width:100%;padding:10px;margin:6px 0;"
"font-size:16px;box-sizing:border-box}button{background:#0a7d5a;color:#fff;"
"border:0;border-radius:6px}label{display:block;margin-top:10px;color:#444}</style>"
"</head><body>"
"<h1>WiFi 配网</h1>"
"<p>选择或输入要连接的 WiFi：</p>"
"<select id=\"list\"><option value=\"\">-- 点击下方按钮扫描 --</option></select>"
"<button onclick=\"scan()\">扫描附近 WiFi</button>"
"<form method=\"POST\" action=\"/save\">"
"<label>WiFi 名称 (SSID)</label><input name=\"ssid\" id=\"ssid\" required>"
"<label>密码</label><input name=\"pass\" type=\"password\">"
"<input type=\"hidden\" name=\"end\" value=\"1\">"
"<button type=\"submit\">保存并重启</button>"
"</form>"
"<script>"
"function scan(){var s=document.getElementById('list');"
"s.innerHTML='<option>扫描中...</option>';"
"fetch('/scan').then(function(r){return r.json();}).then(function(a){"
"s.innerHTML='';if(!a.length){s.innerHTML='<option value=\"\">未发现网络</option>';return;}"
"for(var i=0;i<a.length;i++){var o=document.createElement('option');"
"o.value=a[i].s;o.textContent=a[i].s+' ('+a[i].r+'dBm)';s.appendChild(o);}"
"s.onchange=function(){document.getElementById('ssid').value=s.value;};});}"
"scan();"
"</script></body></html>";

static const char kSavedHtml[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
"<body style=\"font-family:sans-serif;margin:24px\">"
"<h2>保存成功</h2><p>设备即将重启并使用新的 WiFi。</p></body></html>";

/* --------------------------------------------------- +IPD,<id>,<len>: 解析器 */
/* 状态机跨多次调用、以及两条喂入路径（主动读 / AT 应答旁路）之间共享，
   请求体跨帧也能拼齐。 */
enum { PS_SEEK = 0, PS_ID, PS_LEN, PS_DATA };

static int      s_psState;
static uint8_t  s_psMatch;   /* "+IPD," 已匹配的字符数 */
static int      s_psId;      /* 当前帧的 link id */
static uint32_t s_psLen;     /* 当前帧载荷长度 */
static uint32_t s_psGot;     /* 当前帧已收字节数 */

/* 一个 HTTP 请求（头 + 体）暂存区；静态分配，不进栈（任务栈只有 2 KB）。
   必须放得下**最长的请求头**：手机版 Chrome 的 POST /save 请求头带 User-Agent /
   sec-ch-ua / Content-Type 等，实测可达 ~600 B。早期用 512 B 时请求被截断在结尾的
   \r\n\r\n 之前，prov_request_complete() 永远返回 0，表现为"点保存没反应、不重启"。 */
static char s_req[1024];

/* 请求拼装进度：**必须静态**，跨调用保留。
   ESP8266 服务器模式会把一个 HTTP 请求拆成多个 `+IPD,<id>,<len>:` 帧，帧之间
   可能间隔超过一次调用的超时。若把累计长度做成局部变量，下一次调用就从 0 重来，
   于是"后半截请求"被误当成一个新请求（实测 req 只有 85 B），路由落空、真正的
   POST /save 丢失。所以累计长度 + 当前请求的 link id 都放静态。 */
static uint16_t s_reqLen;    /* s_req 中已累计的字节数 */
static int      s_reqId;     /* s_req 正在拼的这个请求来自哪个 link */
static uint8_t  s_reqReady;  /* s_req 里已凑好一个完整请求，等待上层处理 */

/* ------------------------------------------------ 凭据扫描器（兜底，不依赖 HTTP 分帧）
 * 为什么需要它：配网时 HTTP 请求和 AT 应答共用一条 512 B 接收环，发 AT 命令
 * （等 ">" / "SEND OK" / 扫描）时请求的**前半段（含请求行与大多请求头）会被吃掉**，
 * 只剩表单正文 `ssid=..&pass=..` 稳定到达（它在请求最末尾，见历次日志）。
 * 与其死磕 +IPD/HTTP 分帧，不如直接在字节流里找 `ssid=` / `pass=` / `end=1` 三个
 * 标记——表单正文是固定的、位置在末尾，几乎不会被吃掉。只要这三个都到齐就保存。
 * 网页表单里因此加了一个隐藏字段 `end=1` 当结束哨兵（否则最后一个字段没有终止符）。
 */
static uint8_t  s_scanCap;                 /* 0=不在取值 1=正在取 ssid 2=正在取 pass */
static uint8_t  s_scanSsidMatch;           /* "ssid=" 已匹配字符数 */
static uint8_t  s_scanPassMatch;           /* "pass=" 已匹配字符数 */
static uint8_t  s_scanEndMatch;            /* "end=1" 已匹配字符数 */
static uint16_t s_scanSsidLen;
static uint16_t s_scanPassLen;
static char     s_scanSsid[ESP_PROV_SSID_MAX];
static char     s_scanPass[ESP_PROV_PASS_MAX];
static uint8_t  s_scanReady;               /* 三标记到齐，等待主循环保存 */

/* ---------------------------------------------------------------- 小工具 */

/** @brief 十六进制字符转数值；非法返回 -1 */
static int prov_hex(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return c - 'a' + 10; }
    if ((c >= 'A') && (c <= 'F')) { return c - 'A' + 10; }
    return -1;
}

/** @brief 就地解开表单的 URL 编码（`%XX` → 字节，`+` → 空格） */
static void prov_url_decode(char *s)
{
    char *o = s;

    while (*s != '\0')
    {
        if (*s == '+')
        {
            *o++ = ' ';
            s++;
        }
        else if ((*s == '%') && (s[1] != '\0') && (s[2] != '\0'))
        {
            int hi = prov_hex(s[1]);
            int lo = prov_hex(s[2]);

            if ((hi >= 0) && (lo >= 0))
            {
                *o++ = (char)((hi << 4) | lo);
                s += 3;
            }
            else
            {
                *o++ = *s++;
            }
        }
        else
        {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/**
 * @brief  从表单正文（"ssid=..&pass=.."）里取某个字段并做 URL 解码。
 * @retval 0 = 找到；-1 = 没有该字段
 */
static int prov_form_get(const char *body, const char *key, char *out, uint16_t out_size)
{
    size_t      klen = strlen(key);
    const char *p    = body;

    while ((p = strchr(p, '=')) != NULL)
    {
        const char *start = p;
        uint16_t    o     = 0U;

        /* 回退到本字段名起点（上一个 '&' 之后） */
        while ((start > body) && (*(start - 1) != '&')) { start--; }

        if (((size_t)(p - start) == klen) && (memcmp(start, key, klen) == 0))
        {
            p++;   /* 跳过 '=' */
            while ((*p != '\0') && (*p != '&') && (o < (uint16_t)(out_size - 1U)))
            {
                char c = *p++;

                if (c == '+')
                {
                    c = ' ';
                }
                else if ((c == '%') && (p[0] != '\0') && (p[1] != '\0'))
                {
                    int hi = prov_hex(p[0]);
                    int lo = prov_hex(p[1]);

                    if ((hi >= 0) && (lo >= 0))
                    {
                        c = (char)((hi << 4) | lo);
                        p += 2;
                    }
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

/** @brief 判断一个 HTTP 请求是否已收全（头收齐，且 POST 正文按 Content-Length 到齐） */
static int prov_request_complete(const char *req, uint16_t len)
{
    const char *hdr_end = NULL;
    uint16_t    i;

    for (i = 0U; (uint16_t)(i + 4U) <= len; i++)
    {
        if ((req[i] == '\r') && (req[i + 1U] == '\n') &&
            (req[i + 2U] == '\r') && (req[i + 3U] == '\n'))
        {
            hdr_end = req + i + 4U;
            break;
        }
    }
    if (hdr_end == NULL)
    {
        return 0;
    }

    {
        const char *cl = strstr(req, "Content-Length:");

        if (cl != NULL)
        {
            long n = 0;

            cl += 15;
            while (*cl == ' ') { cl++; }
            while ((*cl >= '0') && (*cl <= '9')) { n = (n * 10) + (*cl - '0'); cl++; }
            if ((uint16_t)((uint16_t)(hdr_end - req) + (uint16_t)n) > len)
            {
                return 0;   /* 正文还没到齐 */
            }
        }
    }
    return 1;
}

/* ------------------------------------------------ 凭据扫描器（不依赖 HTTP 分帧） */
/* 直接在原始字节流里找 `ssid=` / `pass=` / `end=1` 三个标记，命中即保存。
   为什么需要：配网时 HTTP 请求与 AT 应答共用一条 512 B 接收环，请求的**前半段
   （请求行 + 多数请求头，含 "+IPD,<id>,<len>:" 前缀）经常在发 AT 命令等应答时被
   吃掉，只剩位处请求末尾的表单正文 `ssid=..&pass=..` 稳定到达。死磕 +IPD/HTTP
   分帧永远会栽在这里；改为直接认表单正文，对"前缀丢失 / 跨帧 / link id"全部免疫。
   网页表单末尾加了一个隐藏字段 `end=1` 当结束哨兵（否则最后一个字段没有终止符）。 */
static const char kTokSsid[] = "ssid=";
static const char kTokPass[] = "pass=";
static const char kTokEnd[]  = "end=1";

static void prov_scan_byte(uint8_t b)
{
    if (s_scanReady != 0U)
    {
        return;
    }

    /* —— 取值阶段：把值字节攒进当前字段，遇到字段分隔符 '&' 收尾 —— */
    if (s_scanCap != 0U)
    {
        if (b == (uint8_t)'&')
        {
            if (s_scanCap == 1U) { s_scanSsid[s_scanSsidLen] = '\0'; }
            else                 { s_scanPass[s_scanPassLen] = '\0'; }
            s_scanCap       = 0U;
            s_scanSsidMatch = 0U;   /* 复位各标记匹配器，准备认下一个字段 */
            s_scanPassMatch = 0U;
            s_scanEndMatch  = 0U;
        }
        else if (s_scanCap == 1U)
        {
            if (s_scanSsidLen < (uint16_t)(sizeof(s_scanSsid) - 1U))
            {
                s_scanSsid[s_scanSsidLen++] = (char)b;
            }
        }
        else
        {
            if (s_scanPassLen < (uint16_t)(sizeof(s_scanPass) - 1U))
            {
                s_scanPass[s_scanPassLen++] = (char)b;
            }
        }
        return;
    }

    /* —— 标记匹配阶段：三个标记同时逐字节比对 —— */
    if (b == (uint8_t)kTokSsid[s_scanSsidMatch]) { s_scanSsidMatch++; }
    else { s_scanSsidMatch = (b == (uint8_t)kTokSsid[0]) ? 1U : 0U; }

    if (b == (uint8_t)kTokPass[s_scanPassMatch]) { s_scanPassMatch++; }
    else { s_scanPassMatch = (b == (uint8_t)kTokPass[0]) ? 1U : 0U; }

    if (b == (uint8_t)kTokEnd[s_scanEndMatch]) { s_scanEndMatch++; }
    else { s_scanEndMatch = (b == (uint8_t)kTokEnd[0]) ? 1U : 0U; }

    if (s_scanSsidMatch >= 5U)
    {
        s_scanSsidMatch = 0U;
        s_scanCap       = 1U;   /* 开始取 ssid 的值 */
        s_scanSsidLen   = 0U;
    }
    else if (s_scanPassMatch >= 5U)
    {
        s_scanPassMatch = 0U;
        s_scanCap       = 2U;   /* 开始取 pass 的值 */
        s_scanPassLen   = 0U;
    }
    else if (s_scanEndMatch >= 5U)
    {
        s_scanEndMatch = 0U;
        if (s_scanSsidLen > 0U)   /* ssid 已取到且非空：凭据齐了 */
        {
            prov_url_decode(s_scanSsid);
            prov_url_decode(s_scanPass);
            s_scanReady = 1U;
        }
    }
}

/**
 * @brief  喂一个原始字节进 "+IPD,<id>,<len>:" 状态机；凑满一个 HTTP 请求时置
 *         s_reqReady（请求留在 s_req 里，等上层取走）。
 * @note   **同时被两条路调用**：① prov_pump() 用 esp_at_read() 主动取到的字节；
 *         ② 发 AT 命令时被 esp_at_expect()/清环顺带吃掉的字节（经 esp_at_set_raw_sink
 *         注册的旁路回调送来）。两条路共用同一份状态，所以"发响应的同时到达的下一个
 *         请求"不会再被丢掉。
 *         每个字节同时也喂给 prov_scan_byte()——那是**更可靠的一道兜底**：
 *         即便 +IPD 前缀整段丢失、HTTP 请求拼不齐，只要表单正文到齐就能保存。
 */
static void prov_feed_byte(uint8_t b)
{
    static const char kHdr[] = "+IPD,";

    prov_scan_byte(b);   /* 兜底扫描器：任何一条喂入路径的字节都过一遍 */

    switch (s_psState)
    {
    case PS_SEEK:
        if (b == (uint8_t)kHdr[s_psMatch])
        {
            s_psMatch++;
            if (s_psMatch >= 5U) { s_psMatch = 0U; s_psId = 0; s_psState = PS_ID; }
        }
        else
        {
            s_psMatch = (b == (uint8_t)'+') ? 1U : 0U;
        }
        break;

    case PS_ID:
        if ((b >= (uint8_t)'0') && (b <= (uint8_t)'9'))
        {
            s_psId = (s_psId * 10) + (int)(b - (uint8_t)'0');
        }
        else if (b == (uint8_t)',')
        {
            s_psLen   = 0U;
            s_psState = PS_LEN;
        }
        else
        {
            s_psState = PS_SEEK;
            s_psMatch = (b == (uint8_t)'+') ? 1U : 0U;
        }
        break;

    case PS_LEN:
        if ((b >= (uint8_t)'0') && (b <= (uint8_t)'9'))
        {
            s_psLen = (s_psLen * 10U) + (uint32_t)(b - (uint8_t)'0');
            if (s_psLen > 4000U) { s_psState = PS_SEEK; s_psMatch = 0U; }   /* 异常长度，放弃 */
        }
        else if (b == (uint8_t)':')
        {
            if (s_psLen == 0U)
            {
                s_psState = PS_SEEK;
            }
            else
            {
                /* 上一个请求已凑齐、但主循环还没取走（典型：发 /scan 响应期间
                   浏览器又发来 favicon / 重复扫描）——**新帧优先**：直接丢弃旧请求
                   从头拼这个新的。否则新请求（尤其 POST /save）会被旧请求挡住，
                   整帧丢失，表现为"点保存没反应"。
                   （若上一个只是没拼完的半包且 link 相同，则保留、继续跨帧拼接。） */
                if (s_reqReady != 0U)
                {
                    s_reqReady = 0U;
                    s_reqLen   = 0U;
                }
                else if ((s_reqLen > 0U) && (s_psId != s_reqId))
                {
                    s_reqLen = 0U;   /* 换了 link：上一个没拼完的请求已作废 */
                }
                s_reqId   = s_psId;
                s_psGot   = 0U;
                s_psState = PS_DATA;
            }
        }
        else
        {
            s_psState = PS_SEEK;
            s_psMatch = 0U;
        }
        break;

    case PS_DATA:
    default:
        if (s_reqLen < (uint16_t)(sizeof(s_req) - 1U))
        {
            s_req[s_reqLen++] = (char)b;
        }
        s_psGot++;
        if (s_psGot >= s_psLen)
        {
            s_psState = PS_SEEK;
            s_psMatch = 0U;
            s_req[s_reqLen] = '\0';
            if (prov_request_complete(s_req, s_reqLen) != 0)
            {
                s_reqReady = 1U;   /* 交给上层处理，先别动缓冲 */
            }
            /* 请求可能跨帧（含 POST 正文）：继续等下一帧补齐 */
        }
        break;
    }
}

/**
 * @brief  主动读一批字节喂给状态机，直到凑出一个完整请求或静默超时。
 * @retval 1 = 已凑好一个请求（s_reqReady）；0 = 超时
 */
static uint8_t prov_pump(uint32_t timeout_ms)
{
    uint32_t idle = esp_at_now_ms();

    for (;;)
    {
        uint8_t  chunk[64];
        uint16_t n = esp_at_read(chunk, (uint16_t)sizeof(chunk), 20U);
        uint16_t i;

        /* 诊断：把模块吐给我们的原始字节全打出来。
           这一路不经过 esp_at_expect()，所以 ESP_LOG_AT_TRAFFIC 镜像不到它 ——
           只有这里能看出"客户端到底有没有连上来（0,CONNECT）/ 有没有发数据（+IPD）"。 */
        if (n > 0U)
        {
            idle = esp_at_now_ms();   /* 有数据就续期：别在收包中途判超时 */
            ESP_LOG("[prov rx %u] %.*s\r\n", (unsigned)n, (int)n, (const char *)chunk);
            for (i = 0U; i < n; i++)
            {
                prov_feed_byte(chunk[i]);
            }
        }

        if (s_reqReady != 0U)
        {
            return 1U;
        }
        if ((esp_at_now_ms() - idle) >= timeout_ms)
        {
            return 0U;
        }
    }
}

/* ---------------------------------------------------------------- 发送 / 应答 */

/** @brief 在某个 link 上发一段数据（AT+CIPSEND=<id>,<len> → 等 '>' → 写 → 等 SEND OK）
 *  @note  按 ≤512 B 分块发。老 AT 固件 v1.2.0.0 单次 CIPSEND 太大（如配网页 1376 B）
 *         会直接卡死在"等待载荷"状态、连 SEND OK 都不回，之后所有命令都被当成载荷吞掉。
 *         HTTP 响应体允许分多个 TCP 段，浏览器会自动拼回整包。 */
static int prov_send(int id, const char *data, uint16_t len)
{
    const uint16_t kChunk = 512U;
    uint16_t       off    = 0U;

    if (len == 0U)
    {
        return 0;
    }

    while (off < len)
    {
        uint16_t n = (uint16_t)(len - off);
        char     cmd[32];

        if (n > kChunk)
        {
            n = kChunk;
        }

        (void)snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u", id, (unsigned)n);
        if (esp_at_cmd(cmd, ">", 2000U) != ESP_OK)
        {
            return -1;
        }
        esp_at_write((const uint8_t *)(data + off), n);
        if (esp_at_expect("SEND OK", 5000U) != ESP_OK)
        {
            return -1;
        }
        off = (uint16_t)(off + n);
    }
    return 0;
}

/** @brief 回一个简单的 HTTP 响应（头 + 体分两次发，省一块大缓冲） */
static int prov_reply(int id, const char *status, const char *ctype,
                      const char *body, uint16_t blen)
{
    char hdr[160];
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                         "Connection: close\r\n\r\n",
                         status, ctype, (unsigned)blen);

    if (hlen <= 0)
    {
        return -1;
    }
    if (prov_send(id, hdr, (uint16_t)hlen) != 0)
    {
        return -1;
    }
    if ((blen > 0U) && (prov_send(id, body, blen) != 0))
    {
        return -1;
    }
    return 0;
}

/* 注意：响应头已带 `Connection: close`，浏览器收到后会自己断开；这里**不要**
   再发 AT+CIPCLOSE。多一次 AT 命令 = 多一段"等应答"窗口，期间到达的下一个
   HTTP 请求会被应答读取顺带消耗，反而更容易丢请求。 */

/* ---------------------------------------------------------------- 路由处理 */

/** @brief 往 JSON 串尾追加（越界即截断），返回新的写入位置 */
static uint16_t prov_append(char *out, uint16_t out_size, uint16_t o, const char *s)
{
    while ((*s != '\0') && (o < (uint16_t)(out_size - 1U)))
    {
        out[o++] = *s++;
    }
    out[o] = '\0';
    return o;
}

/** @brief 把 AT+CWLAP 的应答文本解析成 JSON 数组 [{"s":SSID,"r":RSSI,"e":加密}] */
static uint16_t prov_build_scan_json(const char *resp, char *out, uint16_t out_size)
{
    const char *p     = resp;
    uint16_t    o     = 0U;
    uint8_t     first = 1U;

    o = prov_append(out, out_size, o, "[");
    while ((p = strstr(p, "+CWLAP:(")) != NULL)
    {
        char     ssid[ESP_PROV_SSID_MAX];
        char     num[24];
        uint16_t si   = 0U;
        int      ecn  = 0;
        int      rssi = 0;
        int      sign = 1;

        p += 8;   /* 跳过 "+CWLAP:(" */
        while ((*p >= '0') && (*p <= '9')) { ecn = (ecn * 10) + (*p - '0'); p++; }
        if (*p != ',') { continue; }
        p++;
        if (*p != '"') { continue; }
        p++;

        /* SSID：可能含 \" 与 \\ */
        while ((*p != '\0') && (*p != '"') && (si < (ESP_PROV_SSID_MAX - 1U)))
        {
            if ((*p == '\\') && (*(p + 1) != '\0')) { p++; }
            ssid[si++] = *p++;
        }
        ssid[si] = '\0';
        while ((*p != '\0') && (*p != '"')) { p++; }   /* SSID 超长时跳到结束引号 */
        if (*p == '"') { p++; }
        if (*p != ',') { continue; }
        p++;
        if (*p == '-') { sign = -1; p++; }
        while ((*p >= '0') && (*p <= '9')) { rssi = (rssi * 10) + (*p - '0'); p++; }
        rssi *= sign;

        if (first == 0U) { o = prov_append(out, out_size, o, ","); }
        first = 0U;
        o = prov_append(out, out_size, o, "{\"s\":\"");
        {
            const char *q = ssid;

            while ((*q != '\0') && (o < (uint16_t)(out_size - 2U)))
            {
                if ((*q == '"') || (*q == '\\')) { out[o++] = '\\'; }
                out[o++] = *q++;
            }
            out[o] = '\0';
        }
        (void)snprintf(num, sizeof(num), "\",\"r\":%d,\"e\":%d}", rssi, ecn);
        o = prov_append(out, out_size, o, num);

        if (o >= (uint16_t)(out_size - 24U))
        {
            break;   /* 缓冲快满，收手 */
        }
    }
    o = prov_append(out, out_size, o, "]");
    return o;
}

/** @brief GET / → 返回配网页 */
static void prov_handle_root(int id)
{
    (void)prov_reply(id, "200 OK", "text/html; charset=utf-8",
                     kIndexHtml, (uint16_t)(sizeof(kIndexHtml) - 1U));
}

/** @brief GET /scan → 扫描附近 WiFi 并返回 JSON 列表 */
static void prov_handle_scan(int id)
{
    static char resp[512];
    static char json[640];
    uint16_t    jlen;

    esp_at_flush();   /* 丢掉残留，别和扫描应答混在一起 */
    (void)esp_at_query("AT+CWLAP", resp, (uint16_t)sizeof(resp), ESP_PROV_SCAN_TIMEOUT_MS);
    jlen = prov_build_scan_json(resp, json, (uint16_t)sizeof(json));

    ESP_LOG("[prov] scan -> %.*s\r\n", (int)jlen, json);
    /* 显式声明 charset=utf-8：SSID 里的中文是模块原样吐出的 UTF-8 字节，
       不声明时个别浏览器会按本地编码猜，中文就花了。 */
    (void)prov_reply(id, "200 OK", "application/json; charset=utf-8", json, jlen);
}

/** @brief POST /save → 解析 ssid/pass，写凭据；成功返回 0 */
static int prov_handle_save(int id, const char *req)
{
    char   ssid[ESP_PROV_SSID_MAX];
    char   pass[ESP_PROV_PASS_MAX];
    char  *body = strstr(req, "\r\n\r\n");

    if (body == NULL)
    {
        (void)prov_reply(id, "400 Bad Request", "text/plain", "bad request", 11U);
        return -1;
    }
    body += 4;

    if ((prov_form_get(body, "ssid", ssid, (uint16_t)sizeof(ssid)) != 0) || (ssid[0] == '\0'))
    {
        (void)prov_reply(id, "400 Bad Request", "text/plain", "no ssid", 7U);
        return -1;
    }
    (void)prov_form_get(body, "pass", pass, (uint16_t)sizeof(pass));

    if (esp_cred_save(ssid, pass) != 0)
    {
        ESP_LOG("[prov] save failed\r\n");
        (void)prov_reply(id, "500 Internal Server Error", "text/plain", "save failed", 11U);
        return -1;
    }

    ESP_LOG("[prov] saved ssid=\"%s\"\r\n", ssid);
    (void)prov_reply(id, "200 OK", "text/html; charset=utf-8",
                     kSavedHtml, (uint16_t)(sizeof(kSavedHtml) - 1U));
    return 0;
}

/* ---------------------------------------------------------------- 对外入口 */

/** @brief 连发几次 AT，任一应答 OK 就认为模块活着 */
static int prov_probe(void)
{
    uint8_t i;

    for (i = 0U; i < 5U; i++)
    {
        if (esp_at_cmd("AT", NULL, 500U) == ESP_OK)
        {
            return 0;
        }
    }
    return -1;
}

int esp_prov_run(void)
{
    uint32_t deadline;
    int      rc = -1;

    ESP_LOG("\r\n[prov] ============ CONFIG MODE (reqbuf %uB) ============\r\n",
            (unsigned)sizeof(s_req));

    /* ① 保证不在透传模式，模块活着 */
    if (esp_at_is_transparent() != 0U)
    {
        (void)esp_at_exit_transparent();
    }
    if (prov_probe() != 0)
    {
        ESP_LOG("[prov] esp8266 not responding\r\n");
        return -1;
    }

    /* ② AP+STA（扫描要 STA，服务要 AP），开热点 */
    (void)esp_at_cmd("ATE0", NULL, ESP_AT_CMD_TIMEOUT_MS);
    if (esp_at_cmd("AT+CWMODE=3", NULL, ESP_AT_CMD_TIMEOUT_MS) != ESP_OK)
    {
        ESP_LOG("[prov] CWMODE failed\r\n");
        return -1;
    }
    /* 断开可能还在后台重连的 STA，避免它和 SoftAP 抢信道 / 干扰扫描 */
    (void)esp_at_cmd("AT+CWQAP", NULL, 3000U);
    {
        char cmd[96];

        if (ESP_PROV_AP_PASSWORD[0] != '\0')
        {
            (void)snprintf(cmd, sizeof(cmd), "AT+CWSAP=\"%s\",\"%s\",%u,3",
                           ESP_PROV_AP_SSID, ESP_PROV_AP_PASSWORD,
                           (unsigned)ESP_PROV_AP_CHANNEL);
        }
        else
        {
            (void)snprintf(cmd, sizeof(cmd), "AT+CWSAP=\"%s\",\"\",%u,0",
                           ESP_PROV_AP_SSID, (unsigned)ESP_PROV_AP_CHANNEL);
        }
        if (esp_at_cmd(cmd, "OK", 3000U) != ESP_OK)
        {
            ESP_LOG("[prov] CWSAP failed (older firmware may need AT+CWSAP_CUR)\r\n");
        }
    }

    /* ②b 显式确保 AP 侧 DHCP 打开、网关地址正确。
       老固件 v1.2.0.0 的 DHCP 若没起来，手机会拿到 169.254.x.x，HTTP 根本到不了模块。
       两条命令语法在不同固件版本不一样，失败也无妨（当前的问题不在这也会被忽略）。 */
    (void)esp_at_cmd("AT+CWDHCP=1", NULL, ESP_AT_CMD_TIMEOUT_MS);
    (void)esp_at_cmd("AT+CWDHCP=2,1", NULL, ESP_AT_CMD_TIMEOUT_MS);
    (void)esp_at_cmd("AT+CIPAP=\"192.168.4.1\"", NULL, ESP_AT_CMD_TIMEOUT_MS);

    /* ③ 开 TCP 服务器 */
    (void)esp_at_cmd("AT+CIPMODE=0", NULL, ESP_AT_CMD_TIMEOUT_MS);
    if (esp_at_cmd("AT+CIPMUX=1", NULL, ESP_AT_CMD_TIMEOUT_MS) != ESP_OK)
    {
        ESP_LOG("[prov] CIPMUX failed\r\n");
        return -1;
    }
    {
        char cmd[32];

        (void)snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%u", (unsigned)ESP_PROV_HTTP_PORT);
        if (esp_at_cmd(cmd, "OK", 2000U) != ESP_OK)
        {
            ESP_LOG("[prov] CIPSERVER failed\r\n");
            return -1;
        }
    }

    ESP_LOG("[prov] SoftAP \"%s\" up -> http://192.168.4.1/\r\n", ESP_PROV_AP_SSID);

    /* ④ 之后自己解析 +IPD,<id>,<len>:
       关键：注册旁路回调。发 AT 命令（等 ">" / "SEND OK" / CIPCLOSE）时，
       应答读取会把期间到达的字节当噪声吃掉；同一条接收环上跑的正是 HTTP 请求。
       不注册的话，发完响应那一刻浏览器发来的 POST /save 会整段丢失——
       日志里只剩半截请求、永远不重启。回调让这些字节改道进同一个状态机。 */
    esp_at_flush();
    esp_at_set_raw_rx(1U);
    esp_at_set_raw_sink(prov_feed_byte);
    s_psState  = PS_SEEK;
    s_psMatch  = 0U;
    s_reqLen   = 0U;
    s_reqId    = -1;
    s_reqReady = 0U;

    /* 凭据扫描器复位 */
    s_scanCap       = 0U;
    s_scanSsidMatch = 0U;
    s_scanPassMatch = 0U;
    s_scanEndMatch  = 0U;
    s_scanSsidLen   = 0U;
    s_scanPassLen   = 0U;
    s_scanSsid[0]   = '\0';
    s_scanPass[0]   = '\0';
    s_scanReady     = 0U;

    deadline = esp_at_now_ms() + ESP_PROV_TIMEOUT_MS;
    ESP_LOG("[prov] waiting client on http://192.168.4.1/ ...\r\n");
    {
        uint32_t hb = esp_at_now_ms();

        while ((int32_t)(esp_at_now_ms() - deadline) < 0)
        {
            int      id;
            uint16_t rlen;

            if (s_reqReady == 0U)
            {
                (void)prov_pump(1000U);
            }

            /* 扫描器兜底优先：只要表单正文 `ssid=..&pass=..&end=1` 到齐就直接保存，
               完全绕开 +IPD / HTTP 分帧。这条路径对"请求前缀被 AT 应答吃掉"免疫，
               是"点保存没反应"最可靠的修复。 */
            if (s_scanReady != 0U)
            {
                if (esp_cred_save(s_scanSsid, s_scanPass) == 0)
                {
                    ESP_LOG("[prov] saved ssid=\"%s\" (via scan)\r\n", s_scanSsid);
                    rc = 0;
                    break;
                }
                s_scanReady = 0U;   /* 保存失败：清标志允许重试 */
            }

            if (s_reqReady == 0U)
            {
                /* 每 10 s 打一次心跳，说明本循环在跑、只是还没收到客户端数据 */
                if ((esp_at_now_ms() - hb) >= 10000U)
                {
                    hb = esp_at_now_ms();
                    ESP_LOG("[prov] ...still waiting, no client data yet\r\n");
                }
                continue;
            }

            deadline = esp_at_now_ms() + ESP_PROV_TIMEOUT_MS;   /* 有动静就续期 */
            id   = s_reqId;
            rlen = s_reqLen;
            ESP_LOG("[prov] req(%u) link %d: %.48s\r\n", (unsigned)rlen, id, s_req);

            /* 先把缓冲与解析状态复位：处理本请求、发响应的过程中，
               浏览器可能又发来下一个请求，回调会从 0 开始拼新包。
               （各 handler 都在入口就把 s_req 解析成了局部变量，之后才发 AT 命令，
                 所以这里复位不会打断正在处理的请求。） */
            s_reqReady = 0U;
            s_reqLen   = 0U;
            s_reqId    = -1;
            s_psState  = PS_SEEK;
            s_psMatch  = 0U;

            if (strncmp(s_req, "GET /scan", 9U) == 0)
            {
                prov_handle_scan(id);
            }
            else if (strncmp(s_req, "POST /save", 10U) == 0)
            {
                if (prov_handle_save(id, s_req) == 0)
                {
                    rc = 0;
                    break;
                }
            }
            else
            {
                prov_handle_root(id);
            }
        }
    }

    /* ⑤ 收尾：关服务器、退出 raw 接收、恢复单连模式 */
    esp_at_set_raw_sink(NULL);
    (void)esp_at_cmd("AT+CIPSERVER=0", NULL, ESP_AT_CMD_TIMEOUT_MS);
    (void)esp_at_cmd("AT+CIPMUX=0", NULL, ESP_AT_CMD_TIMEOUT_MS);
    esp_at_set_raw_rx(0U);

    if (rc == 0)
    {
        ESP_LOG("[prov] credentials saved, reboot to apply\r\n");
    }
    else
    {
        ESP_LOG("[prov] exit config mode (timeout)\r\n");
    }
    return rc;
}

#else  /* !ESP_PROV_ENABLE */

int esp_prov_run(void)
{
    return -1;   /* 配网被配置宏关掉 */
}

#endif /* ESP_PROV_ENABLE */