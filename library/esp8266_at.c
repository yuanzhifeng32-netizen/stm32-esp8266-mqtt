/**
 * @file    esp8266_at.c
 * @brief   ESP8266 AT 指令驱动实现（见 esp8266_at.h 的说明）
 *
 * 首发协议栈：MIT
 */

#include "esp8266_at.h"
#include "esp_net_config.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ 私有宏 */

/* 应答文本累积缓冲：AT+CWJAP 的应答最长，512 B 足够 */
#define ESP_AT_RESP_BUFFER_SIZE     512U
/* 等应答时每一小轮阻塞多久（ms）。小块读取让超时判断更细，也不会饿死别的任务 */
#define ESP_AT_READ_SLICE_MS        20U

/* ------------------------------------------------------------------ 私有变量 */

/* 移植层回调集合 */
static const esp_port_t *s_port;

/* 接收环形缓冲：中断里 esp_at_input() 写，任务里 esp_at_read() 读。
   head = 读位置(消费者)，tail = 写位置(生产者)。单生产者单消费者，无需加锁。 */
static uint8_t           s_rxRing[ESP_NET_RX_BUFFER_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;

/* 应答累积缓冲（esp_at_expect 用） */
static char     s_response[ESP_AT_RESP_BUFFER_SIZE];
static uint16_t s_responseLen;

/* 是否处于透传模式 */
static uint8_t  s_transparent;

/* 透传模式下对端关掉 TCP 时，模块会回 AT 模式并吐 "CLOSED"。
   这个标志由字节流匹配器置位，供上层做断链判断。
   匹配器是逐字节状态机，所以 "CLOSED" 被拆到两次 read 里也能认出来。 */
static uint8_t  s_closedFlag;
static uint8_t  s_closedMatch;

/** @brief 往 AT 应答缓冲写一个字节（单生产者中断 + 单消费者任务，无需加锁） */
static void at_rx_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_rxTail + 1U) % ESP_NET_RX_BUFFER_SIZE);

    if (next == s_rxHead)
    {
        return;   /* 缓冲满：丢弃该字节，交给上层超时重来 */
    }
    s_rxRing[s_rxTail] = byte;
    s_rxTail = next;      /* 先写数据再更新 tail，保证读者看到数据时 tail 已更新 */
}

/**
 * @brief  只清「AT 应答文本」这条环形缓冲（不动 TCP 载荷缓冲、也不动拆包状态机）。
 * @note   发 AT 指令前需要清掉上一条指令的残留应答，否则可能读到旧 "OK" 误判成功。
 *         但**绝不能**顺手把分帧模式的 TCP 载荷缓冲也清掉：上行发数据要等 ">"，
 *         这段等待期间到达的下行数据已经躺在数据缓冲里，清掉就等于丢下行。
 *         拆包状态机也不能复位——此刻可能正有一帧 "+IPD" 收到一半。
 */
static void at_flush_rx(void)
{
    s_rxHead = s_rxTail;
}

/* ------------------------------------------- 分帧模式：TCP 载荷专用缓冲与拆包器 */
/* 分帧模式下串口上混着 AT 应答文本和 "+IPD,<len>:" 数据帧。下面这套东西在
   "收到字节"的那一刻就把两者分流：AT 文本进 s_rxRing，TCP 载荷进 s_dataRing。
   分流放在这个位置（而不是读的时候再拆）的原因是：读 AT 应答时是按 32 字节
   一块取的，若数据帧和应答文本挨在一起，一整块会被当成应答文本吃掉，
   下行数据就丢了。分流以后，**等 ">" 握手与收下行数据互不干扰**。 */

#if (ESP_LINK_MODE != ESP_LINK_MODE_TRANSPARENT)

#define ESP_AT_IPD_HDR_LEN          5U      /* "+IPD," 这 5 个字符 */

static const char kIpdHdr[ESP_AT_IPD_HDR_LEN] = { '+', 'I', 'P', 'D', ',' };

static uint8_t           s_dataRing[ESP_AT_DATA_BUFFER_SIZE];
static volatile uint16_t s_dataHead;
static volatile uint16_t s_dataTail;

/* 拆包状态机：SEEK 找 "+IPD," → LEN 数长度到 ':' → DATA 搬 <len> 个载荷字节 */
enum { IPD_SEEK = 0, IPD_LEN, IPD_DATA };

static uint8_t  s_ipdState;
static uint8_t  s_ipdMatch;                             /* 已匹配到的头部字符数 */
static uint8_t  s_ipdHold[ESP_AT_IPD_HDR_LEN];          /* 暂存"可能是头部"的字节 */
static uint16_t s_ipdLen;                               /* 本帧载荷长度 */
static uint16_t s_ipdGot;                               /* 本帧已搬走的字节数 */

/** @brief 往 TCP 载荷缓冲写一个字节；满了丢新字节，由协议层超时重来 */
static void at_data_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_dataTail + 1U) % ESP_AT_DATA_BUFFER_SIZE);

    if (next == s_dataHead)
    {
        return;
    }
    s_dataRing[s_dataTail] = byte;
    s_dataTail = next;
}

/** @brief 把暂存的"头部候选"字节当普通 AT 文本吐回去（说明刚才那串不是 "+IPD,"） */
static void at_flush_hold(void)
{
    uint8_t i;

    for (i = 0U; i < s_ipdMatch; i++)
    {
        at_rx_push(s_ipdHold[i]);
    }
    s_ipdMatch = 0U;
}

/** @brief 分帧模式逐字节分流：命中 "+IPD,<len>:" 之后的 <len> 个字节进数据缓冲 */
static void at_ipd_feed(uint8_t byte)
{
    switch (s_ipdState)
    {
    case IPD_SEEK:
        if (byte == (uint8_t)kIpdHdr[s_ipdMatch])
        {
            s_ipdHold[s_ipdMatch] = byte;
            s_ipdMatch++;
            if (s_ipdMatch >= ESP_AT_IPD_HDR_LEN)
            {
                s_ipdMatch = 0U;
                s_ipdLen   = 0U;
                s_ipdState = IPD_LEN;
            }
        }
        else
        {
            at_flush_hold();
            if (byte == (uint8_t)kIpdHdr[0])
            {
                s_ipdHold[0] = byte;
                s_ipdMatch   = 1U;
            }
            else
            {
                at_rx_push(byte);
            }
        }
        break;

    case IPD_LEN:
        if ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9'))
        {
            if (s_ipdLen > 20000U)          /* 异常长度：放弃这一帧，别把 uint16 撑爆 */
            {
                s_ipdState = IPD_SEEK;
                at_rx_push(byte);
            }
            else
            {
                s_ipdLen = (uint16_t)((s_ipdLen * 10U) + (uint16_t)(byte - (uint8_t)'0'));
            }
        }
        else if (byte == (uint8_t)':')
        {
            if (s_ipdLen == 0U)
            {
                s_ipdState = IPD_SEEK;      /* "+IPD,0:" 空帧，忽略 */
            }
            else
            {
                s_ipdGot   = 0U;
                s_ipdState = IPD_DATA;
            }
        }
        else                                /* 不是数字也不是 ':'：不是合法帧头 */
        {
            s_ipdState = IPD_SEEK;
            at_rx_push(byte);
        }
        break;

    case IPD_DATA:
    default:
        at_data_push(byte);
        s_ipdGot++;
        if (s_ipdGot >= s_ipdLen)
        {
            s_ipdLen   = 0U;
            s_ipdGot   = 0U;
            s_ipdState = IPD_SEEK;
        }
        break;
    }
}

#endif /* 非透传模式 */

/* ---------------------------------------------------------------- 私有函数 */

/** @brief 逐字节匹配 "CLOSED"；命中就置标志，并清掉透传状态（模块已自动退回 AT 模式） */
static void esp_at_watch_byte(uint8_t byte)
{
    static const char kPattern[] = "CLOSED";

    /* 只在透传模式里认 "CLOSED"。AT 指令模式下 "CLOSED" 是正常应答文本：
       重连时 AT+CIPCLOSE 关掉旧 link 就会回一行 "CLOSED"，
       如果不加这个判断，会被当成"对端断了"，刚建好链进 online_loop 就立刻返回，
       于是每一轮重连都走一次 "+++" 退出透传 —— 就是那个无限循环的 +++。 */
    if (s_transparent == 0U)
    {
        return;
    }

    if (byte == (uint8_t)kPattern[s_closedMatch])
    {
        s_closedMatch++;
        if (kPattern[s_closedMatch] == '\0')
        {
            s_closedFlag  = 1U;
            s_closedMatch = 0U;
            s_transparent = 0U;   /* TCP 断开后模块自动退出透传 */
        }
    }
    else
    {
        /* 允许重叠匹配：本字节是 'C' 就直接从 1 重新开始 */
        s_closedMatch = (byte == (uint8_t)kPattern[0]) ? 1U : 0U;
    }
}

/** @brief 环形缓冲写指针前进一步 */
static uint16_t esp_at_ring_next(uint16_t pos)
{
    return (uint16_t)((pos + 1U) % ESP_NET_RX_BUFFER_SIZE);
}

/**
 * @brief  把读游标到写指针之间的新数据搬进 dst（并过一遍断链匹配器）。
 * @retval 本次搬走的字节数
 */
static uint16_t esp_at_ring_take(uint8_t *dst, uint16_t max_len)
{
    uint16_t n = 0U;

    while ((s_rxHead != s_rxTail) && (n < max_len))
    {
        uint8_t byte = s_rxRing[s_rxHead];

        esp_at_watch_byte(byte);   /* 所有收到的字节都过一遍断链匹配器 */
        dst[n++] = byte;
        s_rxHead = esp_at_ring_next(s_rxHead);
    }
    return n;
}

/* ---------------------------------------------------------------- 公开函数 */

void esp_at_init(const esp_port_t *port)
{
    s_port = port;

    s_rxHead      = 0U;
    s_rxTail      = 0U;
    s_responseLen = 0U;
    s_response[0] = '\0';
    s_transparent = 0U;
    s_closedFlag  = 0U;
    s_closedMatch = 0U;
#if (ESP_LINK_MODE != ESP_LINK_MODE_TRANSPARENT)
    s_dataHead = 0U;
    s_dataTail = 0U;
    s_ipdState = IPD_SEEK;
    s_ipdMatch = 0U;
    s_ipdLen   = 0U;
    s_ipdGot   = 0U;
#endif

    /* 启动硬件接收（DMA 循环 + 空闲中断）。放在调度器启动前，避免丢上电数据。 */
    if ((s_port != NULL) && (s_port->init != NULL))
    {
        s_port->init();
    }
}

void esp_at_input(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    for (i = 0U; i < len; i++)
    {
#if (ESP_LINK_MODE == ESP_LINK_MODE_TRANSPARENT)
        at_rx_push(data[i]);
#else
        /* 分帧模式：在这里把 AT 应答文本与 "+IPD" 载荷分流到两条缓冲 */
        at_ipd_feed(data[i]);
#endif
    }
}

void esp_at_flush(void)
{
    s_rxHead = s_rxTail;
#if (ESP_LINK_MODE != ESP_LINK_MODE_TRANSPARENT)
    /* 数据缓冲与拆包状态也要一起清：不然后面新链路的第一个字节
       会被上一轮的半截帧头带偏 */
    s_dataHead = s_dataTail;
    s_ipdState = IPD_SEEK;
    s_ipdMatch = 0U;
    s_ipdLen   = 0U;
    s_ipdGot   = 0U;
#endif
}

uint16_t esp_at_read(uint8_t *dst, uint16_t max_len, uint32_t timeout_ms)
{
    uint32_t start;

    if ((dst == NULL) || (max_len == 0U) || (s_port == NULL))
    {
        return 0U;
    }

    start = s_port->tick_ms();

    for (;;)
    {
        uint16_t n = esp_at_ring_take(dst, max_len);

        if (n > 0U)
        {
            return n;
        }
        if ((s_port->tick_ms() - start) >= timeout_ms)
        {
            return 0U;
        }
        s_port->delay_ms(1U);
    }
}

uint16_t esp_at_read_data(uint8_t *dst, uint16_t max_len, uint32_t timeout_ms)
{
#if (ESP_LINK_MODE == ESP_LINK_MODE_TRANSPARENT)
    /* 透传模式：串口上跑的就是 TCP 裸字节流，和 AT 应答共用一条通路 */
    return esp_at_read(dst, max_len, timeout_ms);
#else
    uint32_t start;

    if ((dst == NULL) || (max_len == 0U) || (s_port == NULL))
    {
        return 0U;
    }

    start = s_port->tick_ms();

    for (;;)
    {
        uint16_t n = 0U;

        /* 只从"数据缓冲"取：里面全是拆包后挑出来的 TCP 载荷，不含任何 AT 文本 */
        while ((s_dataHead != s_dataTail) && (n < max_len))
        {
            dst[n++] = s_dataRing[s_dataHead];
            s_dataHead = (uint16_t)((s_dataHead + 1U) % ESP_AT_DATA_BUFFER_SIZE);
        }

        if (n > 0U)
        {
            return n;
        }
        if ((s_port->tick_ms() - start) >= timeout_ms)
        {
            return 0U;
        }
        s_port->delay_ms(1U);
    }
#endif
}

void esp_at_write(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U) || (s_port == NULL) || (s_port->uart_write == NULL))
    {
        return;
    }
    s_port->uart_write(data, len);
}

esp_err_t esp_at_expect(const char *expect, uint32_t timeout_ms)
{
    uint32_t start;

    if (s_port == NULL)
    {
        return ESP_ERR_PARAM;
    }

    start = s_port->tick_ms();

    s_responseLen = 0U;
    s_response[0] = '\0';

    for (;;)
    {
        uint8_t  chunk[32];
        uint16_t n = esp_at_read(chunk, (uint16_t)sizeof(chunk), ESP_AT_READ_SLICE_MS);

        if (n > 0U)
        {
            uint16_t i;

            /* 累积成 C 字符串，方便 strstr 判断 */
            for (i = 0U; i < n; i++)
            {
                if (s_responseLen < (ESP_AT_RESP_BUFFER_SIZE - 1U))
                {
                    s_response[s_responseLen++] = (char)chunk[i];
                    s_response[s_responseLen]   = '\0';
                }
            }

#if ESP_LOG_AT_TRAFFIC
            /* 把模块的原始应答镜像到日志串口，调试时一眼可见 */
            ESP_LOG("%.*s", (int)n, (const char *)chunk);
#endif

            /* 失败判定优先：模块回 ERROR/FAIL 就立刻返回，不必等满超时 */
            if ((strstr(s_response, "ERROR") != NULL) ||
                (strstr(s_response, "FAIL")  != NULL))
            {
                return ESP_ERR_FAIL;
            }

            if (expect != NULL)
            {
                if (strstr(s_response, expect) != NULL)
                {
                    return ESP_OK;
                }
            }
            else if (strstr(s_response, "OK") != NULL)
            {
                return ESP_OK;
            }
        }

        if ((s_port->tick_ms() - start) >= timeout_ms)
        {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t esp_at_cmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    if (cmd == NULL)
    {
        return ESP_ERR_PARAM;
    }

    /* 清掉上一次的残留应答，否则可能读到旧应答里的 OK 而误判成功。
       注意用 at_flush_rx()（只清 AT 文本）而不是 esp_at_flush()：
       分帧模式下后者会连 TCP 载荷缓冲一起丢掉，等于把下行数据吃了。 */
    at_flush_rx();

#if ESP_LOG_AT_TRAFFIC
    ESP_LOG("> %s\r\n", cmd);
#endif

    esp_at_write((const uint8_t *)cmd, (uint16_t)strlen(cmd));
    esp_at_write((const uint8_t *)"\r\n", 2U);

    return esp_at_expect(expect, timeout_ms);
}

esp_err_t esp_at_query(const char *cmd, char *out, uint16_t out_size, uint32_t timeout_ms)
{
    esp_err_t err = esp_at_cmd(cmd, NULL, timeout_ms);

    if ((out != NULL) && (out_size > 0U))
    {
        /* s_response 由 esp_at_expect() 累积，这里拷出去给调用者解析 */
        uint16_t n = 0U;

        while ((s_response[n] != '\0') && (n < (uint16_t)(out_size - 1U)))
        {
            out[n] = s_response[n];
            n++;
        }
        out[n] = '\0';
    }
    return err;
}

esp_err_t esp_at_send(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return ESP_ERR_PARAM;
    }

#if (ESP_LINK_MODE == ESP_LINK_MODE_TRANSPARENT)
    /* 透传模式：串口上跑的就是 TCP，直接写 */
    esp_at_write(data, len);
    return ESP_OK;
#else
    /* 分帧模式：AT+CIPSEND=<len> → 等 ">" → 发数据 → 等 "SEND OK" */
    char cmd[32];
    (void)snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)len);

    if (esp_at_cmd(cmd, ">", 5000U) != ESP_OK)
    {
        return ESP_ERR_FAIL;
    }
    esp_at_write(data, len);
    return esp_at_expect("SEND OK", 5000U);
#endif
}

esp_err_t esp_at_enter_transparent(void)
{
    if (esp_at_cmd("AT+CIPMODE=1", NULL, ESP_AT_CMD_TIMEOUT_MS) != ESP_OK)
    {
        return ESP_ERR_FAIL;
    }
    /* 等到 ">" 就说明模块已经切到透传，后面写的字节全部进 TCP */
    if (esp_at_cmd("AT+CIPSEND", ">", 5000U) != ESP_OK)
    {
        return ESP_ERR_FAIL;
    }
    s_transparent = 1U;
    return ESP_OK;
}

esp_err_t esp_at_exit_transparent(void)
{
    if (s_transparent == 0U)
    {
        return ESP_OK;
    }

    /* "+++" 有前后保护时间要求：发之前静默 20 ms，发之后等 20 ms 再收应答，
       而且不能带 \r\n —— 带了会被当成 TCP 数据发出去。 */
    ESP_LOG("[esp] exit transparent (+++)\r\n");
    s_port->delay_ms(ESP_AT_ESCAPE_GUARD_MS);
    esp_at_flush();
    esp_at_write((const uint8_t *)"+++", 3U);
    s_port->delay_ms(ESP_AT_ESCAPE_GUARD_MS);

    s_transparent = 0U;
    return esp_at_expect("OK", ESP_AT_CMD_TIMEOUT_MS);
}

uint8_t esp_at_is_transparent(void)
{
    return s_transparent;
}

uint8_t esp_at_link_closed(void)
{
    return s_closedFlag;
}

void esp_at_clear_link_closed(void)
{
    s_closedFlag  = 0U;
    s_closedMatch = 0U;
}

void esp_at_hw_reset(void)
{
    /* 模块要重启了，把软件状态一并清干净 */
    s_transparent = 0U;
    s_closedFlag  = 0U;
    s_closedMatch = 0U;
    esp_at_flush();   /* 丢掉复位前的残留字节 */

    if ((s_port == NULL) || (s_port->reset_pin == NULL))
    {
        return;       /* 没接使能脚就跳过硬件复位 */
    }

    s_port->reset_pin(0U);
    s_port->delay_ms(ESP_AT_RESET_LOW_MS);
    s_port->reset_pin(1U);
}