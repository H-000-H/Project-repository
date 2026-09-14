/* SPDX-License-Identifier: Apache-2.0 */
/*
 * interrupt_stm32.c — STM32F407 板级中断强符号
 *
 * 覆盖 mini_tree weak: interrupt_hw_enable/disable。
 * 提供 HAL 时基 SysTick_Handler，以及调度器 TIM7 → VIRQ(tim,0) 分发；
 * CONFIG_USB 时提供 OTG_FS → TinyUSB 中断分发。
 */
#include "interrupt.h"
#include "hal_systick.h"
#include "stm32f4xx_hal.h"
#ifdef CONFIG_USB
#include "usb_tusb_port.h"
#endif

#if defined(CONFIG_OS_MINI_OS)
#include "port.h"     /* pendsv_handler (符号在 port.S) */
#include "schedule.h" /* mini_os_systick_handler */
#elif defined(CONFIG_OS_FREERTOS)
#include "FreeRTOS.h"
#include "task.h" /* xPortSysTickHandler (port.c 提供) */
#endif

/**
 * @brief 使能 NVIC 中断并设置抢占优先级
 * @param irqn 硬件 IRQn（<0 则忽略）
 * @param priority 抢占优先级
 */
void interrupt_hw_enable(int irqn, uint32_t priority)
{
    if (irqn < 0)
    {
        return;
    }
    NVIC_SetPriority((IRQn_Type)irqn, priority);
    NVIC_EnableIRQ((IRQn_Type)irqn);
}

/**
 * @brief 关闭 NVIC 中断
 * @param irqn 硬件 IRQn（<0 则忽略）
 */
void interrupt_hw_disable(int irqn)
{
    if (irqn < 0)
    {
        return;
    }
    NVIC_DisableIRQ((IRQn_Type)irqn);
}

/**
 * @brief SysTick — HAL 时基 + 内核/调度器滴答钩子
 * @note  强符号覆盖 hal_systick.c 的 weak 版本。
 *        裸机: 链到 hal_systick_irq_handler(), 否则 xtask 累加不到滴答;
 *        mini-os: 链到 mini_os_systick_handler(), 否则内核 tick 停摆;
 *        FreeRTOS: 链到 xPortSysTickHandler() (port.c 提供, 向量表名与它不同名,
 *                  必须由板级转发) 否则任务永远不被切换。
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
#if defined(CONFIG_OS_MINI_OS)
    mini_os_systick_handler();
#elif defined(CONFIG_OS_FREERTOS)
    xPortSysTickHandler();
#else
    hal_systick_irq_handler();
#endif
}

#if defined(CONFIG_OS_MINI_OS)
/**
 * @brief PendSV — mini-os 上下文切换
 * @note  内核符号是**小写**的 pendsv_handler (port.S), 这里做向量表间接层。
 */
void PendSV_Handler(void)
{
    pendsv_handler();
}

/**
 * @brief SVC — mini-os 的 SVC 异常入口
 * @note  同上: 内核符号是小写的 svc_handler (port.S)。
 *        不接线时向量表里 SVC_Handler 会落到启动文件的 weak 定义
 *        (Default_Handler 死循环), 一旦有 SVC 触发就再也出不来。
 */
void SVC_Handler(void)
{
    svc_handler();
}
#endif

/**
 * @brief TIM7 更新中断 → 时间片调度 VIRQ(tim, 0)
 */
void TIM7_IRQHandler(void)
{
    interrupt_virtual_dispatch((uint16_t)VIRQ(tim, 0));
}

#ifdef CONFIG_USB
/**
 * @brief OTG_FS 全局中断 → TinyUSB 协议栈 (rhport 0)
 */
void OTG_FS_IRQHandler(void)
{
    usb_tusb_int_handler(0);
}
#endif /* CONFIG_USB */
