/**
 * @file    demo_app.h
 * @brief   示例业务：周期上报 + 下行命令（框架 API 用法演示）
 *
 * 首发协议栈：MIT
 */

#ifndef DEMO_APP_H
#define DEMO_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  示例业务任务入口（不返回）：每秒跑一次，
 *         联网后按周期把 JSON 上报到 DEMO_TOPIC_PUB。
 * @note   参数与 esp_net_task() 一样，直接作为 FreeRTOS 任务入口使用。
 *         把网络交给 esp_net_task() 之后，你自己的任务就是这样"想干嘛干嘛"。
 */
void demo_app_task(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_APP_H */