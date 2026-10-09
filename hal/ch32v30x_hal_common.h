/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file ch32v30x_hal_common.h
 *@brief CH32V307 mini_tree HAL 强符号实现 — 平台层内部公共辅助 (不对外安装)
 *@author H-000-H
 *@details
 *   分层约束: 本头仅供 hal/*.c 平台实现内部使用; 不暴露给 board/VFS/应用层。
 *   设计约定 (与 mini_tree/hal 各头注释一致):
 *   - cfg 字段由 DTSI 直投 WCH 宏值 (GPIOMode_TypeDef / USART_* / SPI_* / TIM_* 等),
 *     HAL 零翻译透传, 不做 enum 映射;
 *   - 错误码统一 status.h 的 MINI_OK / MINI_ERR_*;
 *   - 超时按 SystemCoreClock 折算空转预算 (单核裸机, 不依赖 tick);
 *   - 中断 / 从机类接口本板未接线, 统一返回 MINI_ERR_NOTSUPP;
 *     DMA 按各模块内联的 DMA1 固定映射实现 (无映射外设返回 MINI_ERR_NOTSUPP)。
 */

#ifndef CH32V30X_HAL_COMMON_H
#define CH32V30X_HAL_COMMON_H

#include "ch32v30x.h"
#include "status.h"
#include "system_ch32v30x.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ------------------------------------------------------------------------ */
    /* 超时预算                                                                  */
    /* ------------------------------------------------------------------------ */

    /**
     * @brief timeout_ms → 空转轮询预算
     * @param[in] timeout_ms 超时毫秒数 (0 = 至少试一次)
     * @return 允许的空转圈数上限 (按每圈约 4 个内核周期估算)
     */
    static inline uint32_t ch32_poll_budget(uint32_t timeout_ms)
    {
        if (timeout_ms == 0U)
            return 1U;
        uint64_t budget = ((uint64_t)SystemCoreClock / 4U) * timeout_ms / 1000U;
        if (budget > 0xFFFFFFFEU)
            return 0xFFFFFFFEU;
        return (uint32_t)budget;
    }

    /* ------------------------------------------------------------------------ */
    /* 引脚归一                                                                  */
    /* ------------------------------------------------------------------------ */

    /**
     * @brief 引脚值归一为位掩码 (兼容 GPIO_Pin_x 位掩码与 0..15 引脚索引)
     * @param[in] pin DTS 直投的引脚值
     * @return 位掩码 (0 = 无引脚)
     */
    static inline uint32_t ch32_pin_to_mask(uint32_t pin)
    {
        if (pin == 0U)
            return 0U;
        if ((pin <= 15U) && ((pin & (pin - 1U)) != 0U))
            return (uint32_t)(1U << pin);
        return pin;
    }

    /**
     * @brief 单脚位掩码 → 引脚索引
     * @param[in] mask 位掩码 (须恰含一个 0..15 位)
     * @param[out] idx 回传引脚索引
     * @return 成功返回 MINI_OK, 多脚/越界返回 MINI_ERR_INVAL
     */
    static inline int ch32_mask_to_index(uint32_t mask, uint32_t* idx)
    {
        if ((mask == 0U) || ((mask & ~0xFFFFU) != 0U) || ((mask & (mask - 1U)) != 0U))
            return MINI_ERR_INVAL;
        *idx = (uint32_t)__builtin_ctz(mask);
        return MINI_OK;
    }

    /* ------------------------------------------------------------------------ */
    /* 时钟树                                                                    */
    /* ------------------------------------------------------------------------ */

    /**
     * @brief 按外设基址所属总线开时钟
     * @param[in] base 外设寄存器基址
     * @param[in] periph_bit RCC_APBxPeriph_xxx 时钟位 (0 = 视为板级已开启)
     * @return 成功返回 MINI_OK
     */
    static inline int ch32_rcc_enable(uintptr_t base, uint32_t periph_bit)
    {
        if (periph_bit == 0U)
            return MINI_OK;
        if ((base >= APB2PERIPH_BASE) && (base < AHBPERIPH_BASE))
            RCC_APB2PeriphClockCmd(periph_bit, ENABLE);
        else if ((base >= APB1PERIPH_BASE) && (base < APB2PERIPH_BASE))
            RCC_APB1PeriphClockCmd(periph_bit, ENABLE);
        else
            RCC_AHBPeriphClockCmd(periph_bit, ENABLE);
        return MINI_OK;
    }

    /**
     * @brief GPIO 端口时钟使能 (clk_bus 缺失时按端口基址查表回退)
     * @param[in] port_base GPIOx_BASE
     * @param[in] clk_bus RCC_APB2Periph_GPIOx (0 = 查表)
     * @return 成功返回 MINI_OK, 端口未知返回 MINI_ERR_INVAL
     */
    static inline int ch32_gpio_port_clk(uintptr_t port_base, uint32_t clk_bus)
    {
        static const uintptr_t s_port_bases[] = {GPIOA_BASE, GPIOB_BASE, GPIOC_BASE, GPIOD_BASE,
                                                 GPIOE_BASE};
        static const uint32_t s_port_bits[] = {RCC_APB2Periph_GPIOA, RCC_APB2Periph_GPIOB,
                                               RCC_APB2Periph_GPIOC, RCC_APB2Periph_GPIOD,
                                               RCC_APB2Periph_GPIOE};
        uint32_t bit = clk_bus;
        if (bit == 0U)
        {
            for (size_t i = 0; i < (sizeof(s_port_bases) / sizeof(s_port_bases[0])); i++)
            {
                if (s_port_bases[i] == port_base)
                {
                    bit = s_port_bits[i];
                    break;
                }
            }
            if (bit == 0U)
                return MINI_ERR_INVAL;
        }
        RCC_APB2PeriphClockCmd(bit, ENABLE);
        return MINI_OK;
    }

    /**
     * @brief 读取外设所在 APB 总线实际频率 (考虑 AHB/APB 分频)
     * @param[in] base 外设寄存器基址 (APB1/APB2 均可)
     * @return 总线频率 (Hz)
     */
    static inline uint32_t ch32_apb_freq(uintptr_t base)
    {
        /* HPRE[3:0]: 0..7=不分频, 8=/2, 9=/4, 10=/8, 11=/16, 12=/64, 13=/128, 14=/256, 15=/512 */
        static const uint8_t s_ahb_shift[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
        uint32_t cfgr0 = RCC->CFGR0;
        uint32_t hclk = SystemCoreClock >> s_ahb_shift[(cfgr0 >> 4) & 0xFU];
        uint32_t ppre_bits;
        if ((base >= APB2PERIPH_BASE) && (base < AHBPERIPH_BASE))
            ppre_bits = (cfgr0 >> 11) & 0x7U; /* PPRE2 */
        else
            ppre_bits = (cfgr0 >> 8) & 0x7U; /* PPRE1 */
        if (ppre_bits >= 4U)
            hclk >>= (ppre_bits - 3U);
        return hclk;
    }

    /**
     * @brief 定时器输入时钟 (APB 分频非 1 时为 APB 时钟的 2 倍)
     * @param[in] tim_base TIMx_BASE
     * @return 定时器计数时钟 (Hz)
     */
    static inline uint32_t ch32_tim_input_freq(uintptr_t tim_base)
    {
        uint32_t cfgr0 = RCC->CFGR0;
        uint32_t ppre_bits;
        if ((tim_base >= APB2PERIPH_BASE) && (tim_base < AHBPERIPH_BASE))
            ppre_bits = (cfgr0 >> 11) & 0x7U;
        else
            ppre_bits = (cfgr0 >> 8) & 0x7U;
        uint32_t pclk = ch32_apb_freq(tim_base);
        return (ppre_bits >= 4U) ? (pclk * 2U) : pclk;
    }

    /* ------------------------------------------------------------------------ */
    /* GPIO 直投配置                                                             */
    /* ------------------------------------------------------------------------ */

    /**
     * @brief 按 WCH 宏直投配置单个/多个 GPIO 引脚
     * @param[in] port_base GPIOx_BASE
     * @param[in] pin_mask 引脚位掩码
     * @param[in] mode GPIOMode_TypeDef 值 (DTS 直投)
     * @param[in] speed GPIOSpeed_TypeDef 值 (0 = 默认 50MHz)
     * @param[in] pull 上下拉直投值 (bit2 = ODR 方向, 1=上拉; 仅 IPD/IPU 模式生效)
     * @return 成功返回 MINI_OK
     */
    static inline int ch32_gpio_cfg_pin(uintptr_t port_base, uint32_t pin_mask, uint32_t mode,
                                        uint32_t speed, uint32_t pull)
    {
        GPIO_TypeDef* port = (GPIO_TypeDef*)port_base;
        GPIO_InitTypeDef gi;
        gi.GPIO_Pin = (uint16_t)pin_mask;
        gi.GPIO_Speed = (speed != 0U) ? (GPIOSpeed_TypeDef)speed : GPIO_Speed_50MHz;
        gi.GPIO_Mode = (GPIOMode_TypeDef)mode;
        GPIO_Init(port, &gi);
        if ((mode == (uint32_t)GPIO_Mode_IPU) || (mode == (uint32_t)GPIO_Mode_IPD))
        {
            if ((pull & 0x04U) != 0U)
                port->BSHR = pin_mask;
            else
                port->BCR = pin_mask;
        }
        return MINI_OK;
    }

#ifdef __cplusplus
}
#endif

#endif /* CH32V30X_HAL_COMMON_H */
