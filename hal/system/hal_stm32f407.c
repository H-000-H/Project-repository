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

    MINI_MEM_COPY(buf, (const void*)addr, len);
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

/**
 * @brief 判断当前是否处于中断上下文
 * @return 非零表示在 ISR 中, 0 表示线程/主上下文
 * @note 实现自 hal_amp.h (原 inline 版), 此处提供外部定义供 mini_tree 链接
 */
int hal_is_in_isr(void)
{
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__) || \
    defined(__ARM_ARCH_8M_MAIN__)
    int ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr;
#elif defined(__riscv)
    int mcause;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    return mcause;
#else
    return 0;
#endif
}

/* ==========================================================================
 * 临时调试: 异常现场存档 (定位 lv_init 里的 UsageFault; 定位完可整段删除)
 * 触发后不再返回, 把异常帧 (压在被中断栈上的 R0/R1/R2/R3/R12/LR/PC/xPSR) 存到这些全局
 * 变量里, 然后用 Renode 读内存 / GDB 看 g_fault_pc 即可知道出事指令地址
 * ========================================================================== */
volatile uint32_t g_fault_pc   = 0U; /**< 出事的 PC (异常帧里的 PC) */
volatile uint32_t g_fault_lr   = 0U; /**< 出事时的 LR (函数返回地址) */
volatile uint32_t g_fault_sp   = 0U; /**< 异常帧所在栈 (PSP 或 MSP) */
volatile uint32_t g_fault_xpsr = 0U; /**< 出事时的 xPSR */
volatile uint32_t g_fault_cfsr = 0U; /**< SCB->CFSR (故障原因位) */
volatile uint32_t g_fault_exc  = 0U; /**< EXC_RETURN (判断用的是 PSP 还是 MSP) */
volatile uint32_t g_fault_src  = 0U; /**< 哪个异常进来的: 1=Hard 2=Usage 3=Bus 4=MemManage */

static void fault_save(uint32_t exc_return, uint32_t src)
{
    /* bit2 = 0 → 用的是 MSP; = 1 → 用的是 PSP */
    const uint32_t sp = ((exc_return & 0x4U) != 0U) ? __get_PSP() : __get_MSP();
    const volatile uint32_t* frame = (const volatile uint32_t*)sp;

    g_fault_src  = src;
    g_fault_exc  = exc_return;
    g_fault_sp   = sp;
    g_fault_pc   = frame[6];
    g_fault_lr   = frame[5];
    g_fault_xpsr = frame[7];
    g_fault_cfsr = SCB->CFSR;

    for (;;)
    {
    }
}

/* 注意: 四个 handler 的体不能完全相同 —— -O2 会把它们合并成一个公共体, 一旦变成
 * "bl 公共体", LR 就被改写成返回地址, 读到的就不再是 EXC_RETURN 了 (踩过)。
 * 所以每个 handler 各给一个不同的 src 常量。LR 必须在本体第一条就读出来。 */
void HardFault_Handler(void)
{
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));
    fault_save(lr, 1U);
}

void UsageFault_Handler(void)
{
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));
    fault_save(lr, 2U);
}

void BusFault_Handler(void)
{
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));
    fault_save(lr, 3U);
}

void MemManage_Handler(void)
{
    uint32_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));
    fault_save(lr, 4U);
}
