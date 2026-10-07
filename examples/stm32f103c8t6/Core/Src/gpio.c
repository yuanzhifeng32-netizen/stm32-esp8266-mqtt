/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* USER CODE BEGIN ESP_EN */
  /* ---- ESP8266 使能脚 PA8：推挽输出，上电先拉低再拉高 ----
     注意：本文件的 `USER CODE BEGIN 2/END 2` 位于 MX_GPIO_Init() **函数之外**，
     代码写在那里会落到文件作用域编译不过，所以这里用一个自定义名的 USER CODE 段。
     .ioc 里没有勾这个脚，写在这里同样不会被重新生成代码覆盖。 */
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = ESP_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;   /* 2 MHz，普通使能/复位信号够用 */
  HAL_GPIO_Init(ESP_EN_GPIO_Port, &GPIO_InitStruct);

  /* 先拉低再拉高，保证 ESP8266 拿到一个干净的复位沿（SPI Flash 启动时更稳） */
  HAL_GPIO_WritePin(ESP_EN_GPIO_Port, ESP_EN_Pin, GPIO_PIN_RESET);
  HAL_Delay(ESP_EN_RESET_LOW_MS);
  HAL_GPIO_WritePin(ESP_EN_GPIO_Port, ESP_EN_Pin, GPIO_PIN_SET);
  /* USER CODE END ESP_EN */

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
