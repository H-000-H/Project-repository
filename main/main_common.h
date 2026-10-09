/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file main_common.h
 *@brief boot(app image_2) 与 app(image_1) 两个入口共用的系统级接口
 *@author H-000-H
 *@details 时钟与错误处理在两个固件里逐字相同, 抽到 main_common.cpp 各编一份。
 */
#ifndef MAIN_COMMON_H
#define MAIN_COMMON_H

#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 系统时钟配置：HSI 16MHz → PLL(M16/N192/P2) → SYSCLK 96MHz
 *        AHB /1，APB1 /4 (24MHz)，APB2 /2 (48MHz)
 */
void SystemClock_Config(void);

/**
 * @brief 错误处理（HAL 返回非 OK 时进入，关中断死循环）
 */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_COMMON_H */
