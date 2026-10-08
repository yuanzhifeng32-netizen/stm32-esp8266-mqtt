/**
 * @file    esp_http.c
 * @brief   极简 HTTP/1.1 客户端实现（见 esp_http.h 的说明）
 *
 * 无 malloc、无第三方依赖；只用 libc 的 snprintf / strstr / strlen（非 HAL、非 RTOS）。
 *
 * 首发协议栈：MIT
 */

#include "esp_http.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ 小工具 */

/** @brief 大小写不敏感地比较前 n 个字符 */
static int http_ci_eq(const char *a, const char *b, uint16_t n)
{
    uint16_t i;

    for (i = 0U; i < n; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if ((ca >= 'A') && (ca <= 'Z')) { ca = (char)(ca - 'A' + 'a'); }
        if ((cb >= 'A') && (cb <= 'Z')) { cb = (char)(cb - 'A' + 'a'); }
        if (ca != cb)
        {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ 公开函数 */

int esp_http_build(const char *method, const char *path,
                   const char *host, uint16_t port, const char *extra_headers,
                   char *out, uint16_t out_size)
{
    int n;

    if ((method == NULL) || (path == NULL) || (host == NULL) ||
        (out == NULL) || (out_size < 16U))
    {
        return -1;
    }

    n = snprintf(out, out_size, "%s %s HTTP/1.1\r\nHost: %s:%u\r\n",
                 method, path, host, (unsigned)port);
    if ((n < 0) || (n >= (int)out_size))
    {
        return -1;
    }

    if (extra_headers != NULL)
    {
        int m = snprintf(&out[n], (size_t)(out_size - (uint16_t)n), "%s", extra_headers);

        if ((m < 0) || (m >= (int)(out_size - (uint16_t)n)))
        {
            return -1;
        }
        n += m;
    }

    {
        int m = snprintf(&out[n], (size_t)(out_size - (uint16_t)n), "\r\n");

        if ((m < 0) || (m >= (int)(out_size - (uint16_t)n)))
        {
            return -1;
        }
        n += m;
    }

    return n;
}

int esp_http_recv(const esp_stream_t *st, char *buf, uint16_t buf_size,
                  uint32_t timeout_ms)
{
    uint16_t total = 0U;

    if ((st == NULL) || (st->read == NULL) || (buf == NULL) || (buf_size < 8U))
    {
        return -1;
    }

    buf[0] = '\0';

    while (total < (uint16_t)(buf_size - 1U))
    {
        int n = st->read((uint8_t *)&buf[total],
                         (uint16_t)(buf_size - 1U - total), timeout_ms);

        if (n < 0)
        {
            return -1;   /* 链路错误 */
        }
        if (n == 0)
        {
            break;       /* 超时没更多数据 */
        }

        total = (uint16_t)(total + (uint16_t)n);
        buf[total] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL)
        {
            return (int)total;   /* 响应头收全了 */
        }
    }

    /* 缓冲满了还没见到空行，或真的没数据 */
    return (strstr(buf, "\r\n\r\n") != NULL) ? (int)total : -1;
}

int esp_http_status(const char *resp)
{
    const char *sp;

    if ((resp == NULL) || (strncmp(resp, "HTTP/", 5U) != 0))
    {
        return -1;
    }

    sp = strchr(resp, ' ');
    if (sp == NULL)
    {
        return -1;
    }
    sp++;

    if ((sp[0] < '0') || (sp[0] > '9') ||
        (sp[1] < '0') || (sp[1] > '9') ||
        (sp[2] < '0') || (sp[2] > '9'))
    {
        return -1;
    }

    return ((sp[0] - '0') * 100) + ((sp[1] - '0') * 10) + (sp[2] - '0');
}

int esp_http_header(const char *resp, const char *name,
                    char *out, uint16_t out_size)
{
    uint16_t    nlen;
    const char *p;

    if ((resp == NULL) || (name == NULL) || (out == NULL) || (out_size == 0U))
    {
        return -1;
    }

    nlen = (uint16_t)strlen(name);
    p    = resp;

    /* 逐行扫描：每行从 '\n' 之后开始，跳过状态行（它前面没有 '\n'） */
    while ((p = strchr(p, '\n')) != NULL)
    {
        const char *line = p + 1;

        if ((http_ci_eq(line, name, nlen) != 0) && (line[nlen] == ':'))
        {
            const char *v = &line[nlen + 1U];
            uint16_t    k = 0U;

            while ((*v == ' ') || (*v == '\t'))
            {
                v++;
            }
            while ((v[k] != '\0') && (v[k] != '\r') && (v[k] != '\n') &&
                   (k < (uint16_t)(out_size - 1U)))
            {
                out[k] = v[k];
                k++;
            }
            out[k] = '\0';
            return (int)k;
        }

        p = line;   /* 继续往后找下一行的 '\n' */
    }

    return -1;   /* 没找到这个头 */
}