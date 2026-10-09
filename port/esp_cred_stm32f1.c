/**
 * @file    esp_cred_stm32f1.c
 * @brief   WiFi 凭据存储的 STM32F1 实现 —— 存在内部 Flash 的最后一页
 *
 * ---------------------------------------------------------------------------
 * 为什么用内部 Flash
 * ---------------------------------------------------------------------------
 *   STM32F103C8T6 没有 EEPROM。好在它自带 64 KB Flash（0x08000000~0x0800FFFF），
 *   程序只占前 ~45 KB，于是把**最后一页**（1 KB，0x0800FC00~0x0800FFFF）腾出来存凭据。
 *
 *   注意：F103C8T6 是 64 KB，最后一页起始是 0x0800FC00（不是 0x0801FC00 —— 那是
 *   128 KB 芯片才有的地址）。写错地址会硬 fault 或静默失败。
 *
 * ---------------------------------------------------------------------------
 * 页内布局（每次保存先擦整页再写）
 * ---------------------------------------------------------------------------
 *   偏移 0   magic     uint32  固定值，用来判断"这一页有没有有效凭据"
 *   偏移 4   ssid_len  uint16  不含结尾 '\0'
 *   偏移 6   pass_len  uint16  不含结尾 '\0'（可为 0 = 开放网络）
 *   偏移 8   cksum     uint16  除本字段外，整块数据的字节和
 *   偏移 12  ssid[]    char[ESP_PROV_SSID_MAX]
 *   偏移 45  pass[]    char[ESP_PROV_PASS_MAX]
 *
 * 首发协议栈：MIT
 */

#include "esp_cred.h"

#include "main.h"      /* HAL：HAL_FLASH_* / FLASH_EraseInitTypeDef */

#include <string.h>

/* 魔数：'W''I''F''1'，用来区分"有效凭据"和"空白 Flash(0xFFFFFFFF)" */
#define CRED_MAGIC          0x57494631U

#define CRED_OFF_MAGIC      0U
#define CRED_OFF_SSIDLEN    4U
#define CRED_OFF_PASSLEN    6U
#define CRED_OFF_CKSUM      8U
#define CRED_OFF_SSID       12U
#define CRED_OFF_PASS       (CRED_OFF_SSID + ESP_PROV_SSID_MAX)

/* 实际写进 Flash 的字节数（110，偶数，正好按半字写） */
#define CRED_BUF_SIZE       (CRED_OFF_PASS + ESP_PROV_PASS_MAX)

/** @brief 整块数据的字节和（跳过 cksum 字段本身，写/读用同一个算法） */
static uint16_t cred_checksum(const uint8_t *p)
{
    uint16_t sum = 0U;
    uint16_t i;

    for (i = 0U; i < CRED_BUF_SIZE; i++)
    {
        if ((i == CRED_OFF_CKSUM) || (i == (CRED_OFF_CKSUM + 1U)))
        {
            continue;
        }
        sum = (uint16_t)(sum + p[i]);
    }
    return sum;
}

int esp_cred_load(esp_cred_t *out)
{
    const uint8_t *p = (const uint8_t *)ESP_CRED_FLASH_PAGE;
    uint32_t       magic;
    uint16_t       ssid_len;
    uint16_t       pass_len;
    uint16_t       cksum;

    if (out == NULL)
    {
        return -1;
    }

    memcpy(&magic,    p + CRED_OFF_MAGIC,   4U);
    memcpy(&ssid_len, p + CRED_OFF_SSIDLEN, 2U);
    memcpy(&pass_len, p + CRED_OFF_PASSLEN, 2U);
    memcpy(&cksum,    p + CRED_OFF_CKSUM,   2U);

    if (magic != CRED_MAGIC)
    {
        return -1;   /* 空白页 / 不是本模块写的 */
    }
    if ((ssid_len == 0U) || (ssid_len >= ESP_PROV_SSID_MAX) ||
        (pass_len >= ESP_PROV_PASS_MAX))
    {
        return -1;
    }
    if (cksum != cred_checksum(p))
    {
        return -1;   /* 数据损坏 */
    }

    memcpy(out->ssid, p + CRED_OFF_SSID, ssid_len);
    out->ssid[ssid_len] = '\0';
    memcpy(out->pass, p + CRED_OFF_PASS, pass_len);
    out->pass[pass_len] = '\0';
    return 0;
}

int esp_cred_save(const char *ssid, const char *pass)
{
    uint8_t                buf[CRED_BUF_SIZE];
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t               page_err = 0U;
    uint32_t               magic = CRED_MAGIC;
    uint16_t               ssid_len;
    uint16_t               pass_len;
    uint16_t               cksum;
    uint16_t               i;
    esp_cred_t             check;

    if ((ssid == NULL) || (pass == NULL))
    {
        return -1;
    }
    ssid_len = (uint16_t)strlen(ssid);
    pass_len = (uint16_t)strlen(pass);
    if ((ssid_len == 0U) || (ssid_len >= ESP_PROV_SSID_MAX) ||
        (pass_len >= ESP_PROV_PASS_MAX))
    {
        return -1;
    }

    /* 先在 RAM 里拼好整块数据，再一次性写进 Flash */
    memset(buf, 0, sizeof(buf));
    memcpy(buf + CRED_OFF_MAGIC,   &magic,    4U);
    memcpy(buf + CRED_OFF_SSIDLEN, &ssid_len, 2U);
    memcpy(buf + CRED_OFF_PASSLEN, &pass_len, 2U);
    memcpy(buf + CRED_OFF_SSID,    ssid,      ssid_len);
    memcpy(buf + CRED_OFF_PASS,    pass,      pass_len);
    cksum = cred_checksum(buf);
    memcpy(buf + CRED_OFF_CKSUM, &cksum, 2U);

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = ESP_CRED_FLASH_PAGE;   /* F1 这里填页起始地址 */
    erase.NbPages     = 1U;
    if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK)
    {
        (void)HAL_FLASH_Lock();
        return -1;
    }

    /* F1 的 Flash 只能按半字（16 位）编程，逐半字写 */
    for (i = 0U; i < CRED_BUF_SIZE; i = (uint16_t)(i + 2U))
    {
        uint16_t half;

        memcpy(&half, buf + i, 2U);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              ESP_CRED_FLASH_PAGE + i, half) != HAL_OK)
        {
            (void)HAL_FLASH_Lock();
            return -1;
        }
    }

    (void)HAL_FLASH_Lock();

    /* 写完读回校验一遍，确保真的落地了（写 Flash 失败很难被上层发现） */
    if (esp_cred_load(&check) != 0)
    {
        return -1;
    }
    if ((strcmp(check.ssid, ssid) != 0) || (strcmp(check.pass, pass) != 0))
    {
        return -1;
    }
    return 0;
}

void esp_cred_erase(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t               page_err = 0U;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return;
    }
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = ESP_CRED_FLASH_PAGE;
    erase.NbPages     = 1U;
    (void)HAL_FLASHEx_Erase(&erase, &page_err);
    (void)HAL_FLASH_Lock();
}