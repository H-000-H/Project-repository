/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hal_stm32f407.c — STM32F407 mini_tree HAL 移植
 *
 * 映射 hal_if 接口到 CMSIS 寄存器 / STM32Cube 外设。
 * 无 WS2812 / pulse engine — 本板未使用该硬件。
 */
#include "hal_platform_safety.h"
#include "hal_amp.h"
#include "hal_flash.h"

#include "stm32f4xx.h"

#include <stddef.h>
#include "compiler_compat.h"
#include "compiler_compat_poison.h"

#ifndef STM32F407_APP_FLASH_BASE
#define STM32F407_APP_FLASH_BASE 0x08000000U
#endif

#ifndef STM32F407_APP_FLASH_SIZE
#define STM32F407_APP_FLASH_SIZE (1024U * 1024U)
#endif

/**
 * @brief 强制停止所有 PWM 输出 (STM32F407 板级空实现)
 */
void hal_pwm_force_stop_all(void)
{
}

/**
 * @brief 紧急停止所有核心 (关中断并死循环)
 */
void hal_cpu_emergency_stop_all_cores(void)
{
    __disable_irq();
    while (1)
    {
        __NOP();
    }
}

/**
 * @brief 从应用 Flash 区读取数据
 * @param addr 起始地址 (须在 app 区范围内)
 * @param buf 输出缓冲
 * @param len 字节数
 * @return 成功返回 true, 参数或范围非法返回 false
 */
bool hal_flash_read(uint32_t addr, uint8_t* buf, size_t len)
{
    if (!buf || len == 0) return false;
    if (addr < STM32F407_APP_FLASH_BASE) return false;
    if ((addr + len) > (STM32F407_APP_FLASH_BASE + STM32F407_APP_FLASH_SIZE)) return false;

    COMPAT_MEM_COPY(buf, (const void*)addr, len);
    return true;
}

/**
 * @brief 获取应用 Flash 起始地址
 * @return 应用区基址
 */
uint32_t hal_flash_get_app_addr(void)
{
    return STM32F407_APP_FLASH_BASE;
}

/**
 * @brief 获取应用 Flash 大小
 * @return 应用区字节数
 */
uint32_t hal_flash_get_app_size(void)
{
    return STM32F407_APP_FLASH_SIZE;
}

/**
 * @brief 平台关键硬件锁定 (当前调用 hal_pwm_force_stop_all)
 */
void hal_platform_critical_hardware_lock(void)
{
    hal_pwm_force_stop_all();
}

/**
 * @brief NMI 紧急标记钩子 (STM32F407 板级空实现)
 */
void hal_platform_nmi_emergency_stamp(void)
{
}

/* ---- 复位向量 SystemInit — 替代 Cube 生成的 system_stm32f4xx.c (Core/ 已移除) ---- */

/**
 * @brief 系统时钟变量 (newlib/HAL 引用) — 默认 HSI 16MHz, 由 main.cpp
 *        SystemClock_Config 完成后更新为实际值 (96MHz)
 */
uint32_t SystemCoreClock = 16000000U;

/**
 * @brief 复位向量入口 (startup_stm32f407xx.s 调用)
 * @note 仅做 FPU 使能 + 默认时钟变量; 完整时钟配置在 main.cpp 的
 *       SystemClock_Config (HAL_RCC) 完成
 */
void SystemInit(void)
{
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    SCB->CPACR |= ((3UL << 10U * 2U) | (3UL << 11U * 2U)); /* FPU 使能 */
#endif
    SystemCoreClock = 16000000U; /* HSI 默认 */
}

const uint8_t AHBPrescTable[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
const uint8_t APBPrescTable[8]  = {0, 0, 0, 0, 1, 2, 3, 4};
