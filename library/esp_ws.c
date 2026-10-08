/**
 * @file    esp_ws.c
 * @brief   WebSocket 客户端实现（见 esp_ws.h 的说明）
 *
 * 组成（按数据流顺序）：
 *   握手：随机 16 字节 → Base64 当 Sec-WebSocket-Key → HTTP GET(Upgrade) → 校验
 *         服务器回的 Sec-WebSocket-Accept = Base64(SHA1(key + RFC6455 GUID))
 *   写：  write() 做"二进制帧 + 客户端掩码"再交给底层
 *   读：  read()  从底层逐帧解析，控制帧(ping/close)就地处理，只把载荷交给上层
 *
 * 无 malloc；SHA-1 / Base64 都是本文件自带的小实现（不为这点功能去拉依赖）。
 * 只支持明文 ws://（ESP8266 AT 固件 v1.2.0.0 无 TLS socket）。
 *
 * 首发协议栈：MIT
 */

#include "esp_ws.h"
#include "esp_http.h"
#include "esp_net_config.h"   /* ESP_WS_* 缓冲大小 / ESP_LOG */

#include <string.h>

/* ---------------------------------------------------------------- 实例状态 */

static const esp_stream_t *s_base;      /* 底层（TCP）字节流 */
static uint8_t             s_ready;     /* 握手是否完成 */

/* 拆帧后攒下的载荷（RX）与"发一帧时给载荷做掩码"的临时区（TX） */
static uint8_t  s_rxBuf[ESP_WS_RX_BUFFER_SIZE];
static uint16_t s_rxLen;                /* s_rxBuf 里有效的载荷字节数 */
static uint16_t s_rxPos;                /* 已被上层取走到哪了 */
static uint8_t  s_txBuf[ESP_WS_TX_BUFFER_SIZE];

/* ------------------------------------------------------------ 小工具：随机 */
/* 只用于生成 Sec-WebSocket-Key 和帧掩码，不要求密码学强度，LCG 足够 */
static uint32_t s_randState = 0x12345678U;

static uint8_t ws_rand8(void)
{
    s_randState = (s_randState * 1664525U) + 1013904223U;
    return (uint8_t)(s_randState >> 24);
}

/* ------------------------------------------------------------ 小工具：Base64 */

/** @brief 把 in[0..n) 做标准 Base64 编码，out 需要 (n+2)/3*4 + 1 字节；返回长度 */
static uint16_t ws_b64(const uint8_t *in, uint16_t n, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint16_t i = 0U;
    uint16_t o = 0U;

    while ((uint16_t)(i + 3U) <= n)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1U] << 8) | in[i + 2U];
        out[o++] = tbl[(v >> 18) & 0x3FU];
        out[o++] = tbl[(v >> 12) & 0x3FU];
        out[o++] = tbl[(v >> 6) & 0x3FU];
        out[o++] = tbl[v & 0x3FU];
        i = (uint16_t)(i + 3U);
    }

    if ((uint16_t)(n - i) == 1U)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = tbl[(v >> 18) & 0x3FU];
        out[o++] = tbl[(v >> 12) & 0x3FU];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if ((uint16_t)(n - i) == 2U)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1U] << 8);
        out[o++] = tbl[(v >> 18) & 0x3FU];
        out[o++] = tbl[(v >> 12) & 0x3FU];
        out[o++] = tbl[(v >> 6) & 0x3FU];
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------ 小工具：SHA-1 */

#define WS_ROL32(x, n) (((x) << (n)) | ((x) >> (32U - (n))))

static void ws_sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    int      i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++)
    {
        w[i] = WS_ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

    for (i = 0; i < 80; i++)
    {
        uint32_t f;
        uint32_t k;
        uint32_t t;

        if (i < 20)      { f = (b & c) | ((~b) & d);    k = 0x5A827999U; }
        else if (i < 40) { f = b ^ c ^ d;               k = 0x6ED9EBA1U; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
        else             { f = b ^ c ^ d;               k = 0xCA62C1D6U; }

        t = WS_ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = WS_ROL32(b, 30);
        b = a;
        a = t;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/** @brief 一次性 SHA-1：输入 data[0..len)，输出 20 字节摘要 */
static void ws_sha1(const uint8_t *data, uint32_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };
    uint8_t  block[64];
    uint32_t i   = 0U;
    uint32_t rem;
    uint64_t bits = (uint64_t)len * 8U;

    while ((len - i) >= 64U)
    {
        ws_sha1_block(h, &data[i]);
        i += 64U;
    }

    rem = len - i;
    memset(block, 0, sizeof(block));
    if (rem > 0U)
    {
        memcpy(block, &data[i], rem);
    }
    block[rem] = 0x80U;

    if (rem >= 56U)
    {
        ws_sha1_block(h, block);
        memset(block, 0, sizeof(block));
    }

    block[56] = (uint8_t)(bits >> 56);
    block[57] = (uint8_t)(bits >> 48);
    block[58] = (uint8_t)(bits >> 40);
    block[59] = (uint8_t)(bits >> 32);
    block[60] = (uint8_t)(bits >> 24);
    block[61] = (uint8_t)(bits >> 16);
    block[62] = (uint8_t)(bits >> 8);
    block[63] = (uint8_t)(bits);
    ws_sha1_block(h, block);

    for (i = 0U; i < 5U; i++)
    {
        out[i * 4U]      = (uint8_t)(h[i] >> 24);
        out[i * 4U + 1U] = (uint8_t)(h[i] >> 16);
        out[i * 4U + 2U] = (uint8_t)(h[i] >> 8);
        out[i * 4U + 3U] = (uint8_t)(h[i]);
    }
}

/* -------------------------------------------------------------- 帧的收发 */

/** @brief 从底层读满 n 个字节；返回 n=读满，0=一开始就没数据，-1=中途断了 */
static int ws_read_full(uint8_t *dst, uint16_t n, uint32_t timeout_ms)
{
    uint16_t got = 0U;

    while (got < n)
    {
        int r = s_base->read(&dst[got], (uint16_t)(n - got), timeout_ms);

        if (r < 0)
        {
            return -1;
        }
        if (r == 0)
        {
            return (got == 0U) ? 0 : -1;   /* 一个字节都没等到 = 超时 */
        }
        got = (uint16_t)(got + (uint16_t)r);
    }
    return (int)got;
}

/** @brief 回一个 pong（载荷通常很小） */
static void ws_send_pong(const uint8_t *payload, uint16_t len)
{
    uint8_t  hdr[6];
    uint8_t  mask[4];
    uint16_t i;

    if ((s_base == NULL) || (len > sizeof(s_txBuf)))
    {
        return;
    }

    hdr[0] = 0x8AU;   /* FIN + opcode pong */

    if (len < 126U)
    {
        hdr[1] = (uint8_t)(0x80U | (uint8_t)len);
    }
    else
    {
        hdr[1] = (uint8_t)(0x80U | 126U);
        hdr[2] = (uint8_t)((len >> 8) & 0xFFU);
        hdr[3] = (uint8_t)(len & 0xFFU);
    }

    for (i = 0U; i < 4U; i++)
    {
        mask[i] = ws_rand8();
    }

    if (len < 126U)
    {
        memcpy(&hdr[2], mask, 4U);
        (void)s_base->write(hdr, 6U);
    }
    else
    {
        memcpy(&hdr[4], mask, 4U);
        (void)s_base->write(hdr, 8U);
    }

    for (i = 0U; i < len; i++)
    {
        s_txBuf[i] = (uint8_t)(payload[i] ^ mask[i & 3U]);
    }
    if (len > 0U)
    {
        (void)s_base->write(s_txBuf, len);
    }
}

/* esp_stream_t::write —— 把一段字节做成一帧二进制消息发出去 */
static int ws_write(const uint8_t *data, uint16_t len)
{
    uint8_t  hdr[8];
    uint8_t  mask[4];
    uint16_t hlen;
    uint16_t i;
    int      n;

    if ((s_ready == 0U) || (s_base == NULL) || (data == NULL))
    {
        return -1;
    }
    if (len > (uint16_t)sizeof(s_txBuf))
    {
        return -1;   /* 单帧太大（正常不会：MQTT TX 缓冲比它小） */
    }

    hdr[0] = 0x82U;   /* FIN=1, opcode=0x2 二进制 */

    if (len < 126U)
    {
        hdr[1] = (uint8_t)(0x80U | (uint8_t)len);
        hlen = 2U;
    }
    else
    {
        hdr[1] = (uint8_t)(0x80U | 126U);
        hdr[2] = (uint8_t)((len >> 8) & 0xFFU);
        hdr[3] = (uint8_t)(len & 0xFFU);
        hlen = 4U;
    }

    for (i = 0U; i < 4U; i++)
    {
        mask[i] = ws_rand8();
    }
    memcpy(&hdr[hlen], mask, 4U);
    hlen = (uint16_t)(hlen + 4U);

    /* 客户端发出的帧必须加掩码；不能改调用者的 const 缓冲，故先拷到 s_txBuf */
    for (i = 0U; i < len; i++)
    {
        s_txBuf[i] = (uint8_t)(data[i] ^ mask[i & 3U]);
    }

    n = s_base->write(hdr, hlen);
    if (n != (int)hlen)
    {
        return -1;
    }
    if (len > 0U)
    {
        n = s_base->write(s_txBuf, len);
        if (n != (int)len)
        {
            return -1;
        }
    }
    return (int)len;
}

/* esp_stream_t::read —— 拆帧：控制帧就地处理，业务载荷交给上层 */
static int ws_read(uint8_t *data, uint16_t max_len, uint32_t timeout_ms)
{
    if ((s_ready == 0U) || (s_base == NULL) || (data == NULL))
    {
        return -1;
    }

    /* ① 上次拆的帧还没交完，先接着交 */
    if (s_rxPos < s_rxLen)
    {
        uint16_t avail = (uint16_t)(s_rxLen - s_rxPos);
        uint16_t n     = (avail < max_len) ? avail : max_len;

        memcpy(data, &s_rxBuf[s_rxPos], n);
        s_rxPos = (uint16_t)(s_rxPos + n);
        return (int)n;
    }

    /* ② 从底层取新的一帧 */
    for (;;)
    {
        uint8_t  hdr[2];
        uint8_t  ext[8];
        uint8_t  mask[4];
        uint8_t  op;
        uint8_t  masked;
        uint64_t plen;
        uint16_t i;
        int      r;

        r = ws_read_full(hdr, 2U, timeout_ms);
        if (r <= 0)
        {
            return r;   /* 0 = 没数据，-1 = 链路错误 */
        }

        op     = (uint8_t)(hdr[0] & 0x0FU);
        masked = (uint8_t)((hdr[1] & 0x80U) ? 1U : 0U);
        plen   = (uint64_t)(hdr[1] & 0x7FU);

        if (plen == 126U)
        {
            r = ws_read_full(ext, 2U, timeout_ms);
            if (r <= 0)
            {
                return -1;
            }
            plen = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
        }
        else if (plen == 127U)
        {
            r = ws_read_full(ext, 8U, timeout_ms);
            if (r <= 0)
            {
                return -1;
            }
            plen = 0U;
            for (i = 0U; i < 8U; i++)
            {
                plen = (plen << 8) | (uint64_t)ext[i];
            }
        }

        if (masked != 0U)
        {
            r = ws_read_full(mask, 4U, timeout_ms);
            if (r <= 0)
            {
                return -1;
            }
        }

        /* ---- 控制帧 ---- */
        if (op == 0x8U)   /* close：对端要断开 */
        {
            ESP_LOG("[ws] server closed the websocket\r\n");
            return -1;
        }
        if ((op == 0x9U) || (op == 0xAU))   /* ping / pong */
        {
            uint8_t tmp[16];

            if (plen > (uint64_t)sizeof(tmp))
            {
                return -1;   /* 正常 ping 载荷极小 */
            }
            if (plen > 0U)
            {
                r = ws_read_full(tmp, (uint16_t)plen, timeout_ms);
                if (r <= 0)
                {
                    return -1;
                }
                if (masked != 0U)
                {
                    for (i = 0U; i < (uint16_t)plen; i++)
                    {
                        tmp[i] = (uint8_t)(tmp[i] ^ mask[i & 3U]);
                    }
                }
            }
            if (op == 0x9U)
            {
                ws_send_pong(tmp, (uint16_t)plen);
            }
            continue;   /* 控制帧不交给上层，继续取下一帧 */
        }

        /* ---- 业务帧：continuation(0x0) / text(0x1) / binary(0x2) 都当字节流 ---- */
        if (plen > (uint64_t)ESP_WS_RX_BUFFER_SIZE)
        {
            ESP_LOG("[ws] frame too big (%u), drop\r\n", (unsigned)plen);
            return -1;
        }

        if (plen > 0U)
        {
            r = ws_read_full(s_rxBuf, (uint16_t)plen, timeout_ms);
            if (r <= 0)
            {
                return -1;
            }
            if (masked != 0U)
            {
                for (i = 0U; i < (uint16_t)plen; i++)
                {
                    s_rxBuf[i] = (uint8_t)(s_rxBuf[i] ^ mask[i & 3U]);
                }
            }
        }

        s_rxPos = 0U;
        s_rxLen = (uint16_t)plen;
        if (s_rxLen == 0U)
        {
            continue;   /* 空帧，跳过 */
        }

        {
            uint16_t n = (s_rxLen < max_len) ? s_rxLen : max_len;

            memcpy(data, s_rxBuf, n);
            s_rxPos = n;
            return (int)n;
        }
    }
}

/* 包装出来的字节流：交给 MQTT 用的就是它，MQTT 不知道底下是 WebSocket */
static const esp_stream_t s_wsStream =
{
    ws_write,
    ws_read,
};

/* ---------------------------------------------------------------- 公开函数 */

void esp_ws_init(const esp_stream_t *base)
{
    s_base  = base;
    s_ready = 0U;
    s_rxLen = 0U;
    s_rxPos = 0U;
}

const esp_stream_t *esp_ws_stream(void)
{
    return &s_wsStream;
}

void esp_ws_reset(void)
{
    s_ready = 0U;
    s_rxLen = 0U;
    s_rxPos = 0U;
}

int esp_ws_connect(const char *host, uint16_t port, const char *path,
                   uint32_t timeout_ms)
{
    uint8_t  key[16];
    char     keyB64[32];
    char     extra[160];
    char     req[320];
    char     guid[80];
    char     expect[40];
    char     accept[48];
    uint8_t  digest[20];
    int      reqLen;
    int      r;
    uint8_t  i;

    if ((s_base == NULL) || (s_base->write == NULL) || (s_base->read == NULL) ||
        (host == NULL) || (path == NULL))
    {
        return -1;
    }

    s_ready = 0U;
    s_rxLen = 0U;
    s_rxPos = 0U;

    /* ① 随机 16 字节 → Base64 作为 Sec-WebSocket-Key */
    for (i = 0U; i < 16U; i++)
    {
        key[i] = ws_rand8();
    }
    (void)ws_b64(key, 16U, keyB64);

    /* ② 拼握手请求（HTTP 部分复用 esp_http） */
    (void)snprintf(extra, sizeof(extra),
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n",
                   keyB64);

    reqLen = esp_http_build("GET", path, host, port, extra, req, (uint16_t)sizeof(req));
    if (reqLen < 0)
    {
        ESP_LOG("[ws] build handshake failed\r\n");
        return -1;
    }

    if (s_base->write((const uint8_t *)req, (uint16_t)reqLen) != reqLen)
    {
        ESP_LOG("[ws] send handshake failed\r\n");
        return -1;
    }

    /* ③ 收响应（复用 RX 缓冲，此时还没有帧数据） */
    r = esp_http_recv(s_base, (char *)s_rxBuf, (uint16_t)sizeof(s_rxBuf), timeout_ms);
    if (r < 0)
    {
        ESP_LOG("[ws] no handshake response\r\n");
        return -1;
    }

    if (esp_http_status((const char *)s_rxBuf) != 101)
    {
        ESP_LOG("[ws] handshake rejected, status %d\r\n",
                esp_http_status((const char *)s_rxBuf));
        return -1;
    }

    /* ④ 校验 Sec-WebSocket-Accept = Base64(SHA1(key + RFC6455 GUID)) */
    (void)snprintf(guid, sizeof(guid),
                   "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", keyB64);
    ws_sha1((const uint8_t *)guid, (uint32_t)strlen(guid), digest);
    (void)ws_b64(digest, 20U, expect);

    if (esp_http_header((const char *)s_rxBuf, "Sec-WebSocket-Accept",
                        accept, (uint16_t)sizeof(accept)) < 0)
    {
        ESP_LOG("[ws] missing Sec-WebSocket-Accept\r\n");
        return -1;
    }
    if (strcmp(accept, expect) != 0)
    {
        ESP_LOG("[ws] accept mismatch (got %s)\r\n", accept);
        return -1;
    }

    s_ready = 1U;
    ESP_LOG("[ws] handshake ok (%s:%u%s)\r\n", host, (unsigned)port, path);
    return 0;
}