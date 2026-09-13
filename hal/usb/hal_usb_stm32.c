/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file  hal_usb_stm32.c
 * @brief USB HAL — STM32F4 实现 (HAL GPIO/RCC/NVIC, 无 LL, 无 HAL_PCD)
 *
 * 职责: 使能 OTG 外设时钟、配置 DP/DM 复用、设置 NVIC。
 * 传输路径由 cfg.dma_cfg.dma_enable 与 xfer_mode 在 resolve 中判定。
 */
#define HAL_USB_IMPL
#include "hal_usb.h"
#include "compiler_compat.h"

#include "stm32f4xx_hal.h"

/**
 * @brief 按 GPIO 端口基址使能 AHB1 时钟
 * @param port GPIOx_BASE
 */
static void hal_usb_gpio_clk_enable(uintptr_t port)
{
    if (port == GPIOA_BASE)
        __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == GPIOB_BASE)
        __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == GPIOC_BASE)
        __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == GPIOD_BASE)
        __HAL_RCC_GPIOD_CLK_ENABLE();
    else if (port == GPIOE_BASE)
        __HAL_RCC_GPIOE_CLK_ENABLE();
    else if (port == GPIOF_BASE)
        __HAL_RCC_GPIOF_CLK_ENABLE();
    else if (port == GPIOG_BASE)
        __HAL_RCC_GPIOG_CLK_ENABLE();
    else if (port == GPIOH_BASE)
        __HAL_RCC_GPIOH_CLK_ENABLE();
    else if (port == GPIOI_BASE)
        __HAL_RCC_GPIOI_CLK_ENABLE();
}

/**
 * @brief 配置单脚为 USB AF 推挽高速
 * @param pin 引脚配置
 * @return MINI_OK 或 MINI_ERR_INVAL
 */
static int hal_usb_config_af_pin(const struct hal_usb_pin_cfg* pin)
{
    GPIO_InitTypeDef init = {0};

    if (!pin || !pin->port || !pin->pin)
        return MINI_ERR_INVAL;

    hal_usb_gpio_clk_enable(pin->port);

    init.Pin       = pin->pin;
    init.Mode      = GPIO_MODE_AF_PP;
    init.Pull      = GPIO_NOPULL;
    init.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = pin->af;
    HAL_GPIO_Init((GPIO_TypeDef*)pin->port, &init);
    return MINI_OK;
}

/**
 * @brief 反初始化单脚 GPIO
 * @param pin 引脚配置
 */
static void hal_usb_reset_pin(const struct hal_usb_pin_cfg* pin)
{
    if (!pin || !pin->port || !pin->pin)
        return;
    HAL_GPIO_DeInit((GPIO_TypeDef*)pin->port, pin->pin);
}

/**
 * @brief 使能 OTG 外设时钟
 * @param usb_base USB_OTG_FS/HS_PERIPH_BASE
 * @return MINI_OK 或 MINI_ERR_INVAL
 */
static int hal_usb_periph_clk_enable(uintptr_t usb_base)
{
    if (usb_base == USB_OTG_FS_PERIPH_BASE)
    {
        __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
        return MINI_OK;
    }
#ifdef USB_OTG_HS_PERIPH_BASE
    if (usb_base == USB_OTG_HS_PERIPH_BASE)
    {
        __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
        return MINI_OK;
    }
#endif
    return MINI_ERR_INVAL;
}

/**
 * @brief 关闭 OTG 外设时钟
 * @param usb_base USB_OTG_FS/HS_PERIPH_BASE
 */
static void hal_usb_periph_clk_disable(uintptr_t usb_base)
{
    if (usb_base == USB_OTG_FS_PERIPH_BASE)
        __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
#ifdef USB_OTG_HS_PERIPH_BASE
    else if (usb_base == USB_OTG_HS_PERIPH_BASE)
        __HAL_RCC_USB_OTG_HS_CLK_DISABLE();
#endif
}

int hal_usb_bus_host_init(struct hal_usb_bus_host* host,
                          const struct hal_usb_bus_config* cfg)
{
    int ret;

    if (!host || !cfg || !cfg->usb_base)
        return MINI_ERR_INVAL;

    MINI_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;

    ret = hal_usb_config_af_pin(&cfg->dp);
    if (ret != MINI_OK)
        return ret;
    ret = hal_usb_config_af_pin(&cfg->dm);
    if (ret != MINI_OK)
    {
        hal_usb_reset_pin(&cfg->dp);
        return ret;
    }

    ret = hal_usb_periph_clk_enable(cfg->usb_base);
    if (ret != MINI_OK)
    {
        hal_usb_reset_pin(&cfg->dp);
        hal_usb_reset_pin(&cfg->dm);
        return ret;
    }

    HAL_NVIC_SetPriority((IRQn_Type)cfg->irqn, 6, 0);
    host->inited = 1;
    return MINI_OK;
}

int hal_usb_bus_host_deinit(struct hal_usb_bus_host* host)
{
    if (!host || !host->inited)
        return MINI_ERR_INVAL;

    hal_usb_irq_disable(host);
    hal_usb_periph_clk_disable(host->cfg.usb_base);
    hal_usb_reset_pin(&host->cfg.dp);
    hal_usb_reset_pin(&host->cfg.dm);
    MINI_MEM_SET(host, 0, sizeof(*host));
    return MINI_OK;
}

void hal_usb_irq_enable(const struct hal_usb_bus_host* host)
{
    if (host && host->inited)
        HAL_NVIC_EnableIRQ((IRQn_Type)host->cfg.irqn);
}

void hal_usb_irq_disable(const struct hal_usb_bus_host* host)
{
    if (host && host->inited)
        HAL_NVIC_DisableIRQ((IRQn_Type)host->cfg.irqn);
}

int hal_usb_resolve_xfer_mode(const struct hal_usb_bus_host* host, uint32_t xfer_mode)
{
    int dma_ok;

    if (!host || !host->inited)
        return MINI_ERR_INVAL;
    if (xfer_mode > HAL_USB_XFER_DMA)
        return MINI_ERR_INVAL;

    dma_ok = host->cfg.dma_cfg.dma_enable ? 1 : 0;

    if (xfer_mode == HAL_USB_XFER_DMA)
    {
        if (!dma_ok)
            return MINI_ERR_NOTSUPP;
        return (int)HAL_USB_XFER_DMA;
    }
    if (xfer_mode == HAL_USB_XFER_POLL)
        return (int)HAL_USB_XFER_POLL;

    /* AUTO */
    return dma_ok ? (int)HAL_USB_XFER_DMA : (int)HAL_USB_XFER_POLL;
}
