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
 * @brief SysTick — HAL 时基 + 调度器滴答钩子
 * @note  强符号覆盖 hal_systick.c 的 weak 版本; 除 HAL 时基外必须链到
 *        hal_systick_irq_handler(), 否则抢占式调度器 (SysTick tick 源) 无法累加滴答。
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
    hal_systick_irq_handler();
}

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
