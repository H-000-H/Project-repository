/* SPDX-License-Identifier: Apache-2.0 */
/*
 * I2C HAL — STM32F1 实现 (HAL 库)
 *
 * 设计: 上层接口 hal_i2c.h 不变, 平台实现直接用 STM32 HAL 库函数。
 * - GPIO(SCL/SDA 开漏复用+上拉) 在 hw_open 前用 HAL_GPIO_Init 配置。
 * - 参数从 hal_i2c_bus_config / hal_i2c_device_config 结构体读出, 填 HAL 结构。
 * - 同步: HAL_I2C_Master_Transmit / HAL_I2C_Master_Receive
 * - DMA : HAL_I2C_Master_Transmit_DMA / HAL_I2C_Master_Receive_DMA
 * - F1 的 HAL_I2C_Master_* 的 DevAddress 需传 8-bit 地址 (7-bit<<1)。
 */
#include "hal_i2c.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_i2c.h"
#include "stm32f1xx_hal_gpio.h"

#define HAL_I2C_F1_INSTANCES 1U

typedef struct hal_i2c_f1_ctx
{
    bool               used;
    struct hal_i2c_bus_host* host;
    I2C_HandleTypeDef  hi2c;
    DMA_HandleTypeDef  hdmatx;
    DMA_HandleTypeDef  hdmarx;
    bool               dma_tx_ok;
    bool               dma_rx_ok;
} hal_i2c_f1_ctx_t;

static hal_i2c_f1_ctx_t s_ctx[HAL_I2C_F1_INSTANCES];

static hal_i2c_f1_ctx_t* hal_i2c_f1_find(I2C_TypeDef* inst)
{
    uint32_t i;
    for (i = 0; i < HAL_I2C_F1_INSTANCES; i++)
        if (s_ctx[i].used && s_ctx[i].hi2c.Instance == inst)
            return &s_ctx[i];
    return NULL;
}

static int hal_i2c_f1_map(HAL_StatusTypeDef st)
{
    switch (st)
    {
    case HAL_OK:      return VFS_OK;
    case HAL_TIMEOUT: return VFS_ERR_TIMEOUT;
    case HAL_ERROR:   return VFS_ERR_IO;
    case HAL_BUSY:    return VFS_ERR_IO;
    default:          return VFS_ERR_IO;
    }
}

/** 按 GPIO 基址使能端口时钟 (F103xB: GPIOA..GPIOE) */
static void hal_i2c_f1_enable_gpio_clock(uintptr_t port)
{
    if (port == GPIOA_BASE)      __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == GPIOB_BASE) __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == GPIOC_BASE) __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == GPIOD_BASE) __HAL_RCC_GPIOD_CLK_ENABLE();
    else if (port == GPIOE_BASE) __HAL_RCC_GPIOE_CLK_ENABLE();
}

/** 按 I2C 基址使能外设时钟 (I2C1/I2C2 = APB1) */
static void hal_i2c_f1_enable_periph_clock(uintptr_t base)
{
    if (base == I2C1_BASE)      __HAL_RCC_I2C1_CLK_ENABLE();
    else if (base == I2C2_BASE) __HAL_RCC_I2C2_CLK_ENABLE();
}

/** 配置 I2C 引脚 (开漏复用 + 上拉, F1 标准) */
static void hal_i2c_f1_config_pin(const struct hal_i2c_pin_cfg* pin)
{
    GPIO_InitTypeDef gi = {0};
    gi.Pin   = pin->pin;
    gi.Mode  = pin->mode ? pin->mode : GPIO_MODE_AF_OD;
    gi.Speed = pin->speed ? pin->speed : GPIO_SPEED_FREQ_HIGH;
    gi.Pull  = pin->pull ? pin->pull : GPIO_PULLUP;
    HAL_GPIO_Init((GPIO_TypeDef*)pin->port, &gi);
}

/** 从 DMA 基址 + 通道号算 F1 DMA 通道实例 */
static DMA_Channel_TypeDef* hal_i2c_f1_dma_channel(uintptr_t dma_base, uint32_t channel)
{
    if (channel < 1U || channel > 7U)
        return NULL;
    return (DMA_Channel_TypeDef*)(dma_base + 0x08U + ((channel - 1U) * 0x14U));
}

/*============================================================================*/
/*                              Bus / Device 管理                             */
/*============================================================================*/

int hal_i2c_bus_host_init(struct hal_i2c_bus_host* host, int hw_idx,
                          const struct hal_i2c_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= (int)HAL_I2C_HOST_MAX)
        return VFS_ERR_INVAL;
    if (host->bus_ready)
        return VFS_OK;
    if (!cfg->i2c)
        return VFS_ERR_NODEV;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;
    if (host->cfg.max_transfer_sz == 0 || host->cfg.max_transfer_sz > HAL_I2C_MAX_XFER)
        host->cfg.max_transfer_sz = HAL_I2C_MAX_XFER;

    /* 外设 + GPIO 时钟 */
    hal_i2c_f1_enable_periph_clock(cfg->i2c);
    hal_i2c_f1_enable_gpio_clock(cfg->scl.port);
    hal_i2c_f1_enable_gpio_clock(cfg->sda.port);

    /* SCL/SDA 开漏复用 + 上拉 */
    hal_i2c_f1_config_pin(&cfg->scl);
    hal_i2c_f1_config_pin(&cfg->sda);

    host->i2c       = cfg->i2c;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return VFS_OK;
}

int hal_i2c_bus_host_deinit(struct hal_i2c_bus_host* host)
{
    hal_i2c_f1_ctx_t* ctx;
    if (!host)
        return VFS_ERR_INVAL;
    if (!host->bus_ready)
        return VFS_OK;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)host->i2c);
    if (ctx)
    {
        HAL_I2C_DeInit(&ctx->hi2c);
        ctx->used = false;
    }
    host->bus_ready = false;
    host->hw_inited = false;
    return VFS_OK;
}

int hal_i2c_dev_init(struct hal_i2c_dev* pdev, struct hal_i2c_bus_host* host,
                     const struct hal_i2c_device_config* dev_cfg)
{
    if (!pdev || !host || !dev_cfg)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->ctlr = host;
    pdev->cfg  = *dev_cfg;
    return VFS_OK;
}

int hal_i2c_dev_deinit(struct hal_i2c_dev* pdev)
{
    if (!pdev)
        return VFS_ERR_INVAL;
    if (pdev->hw_open)
        COMPAT_IGNORE_RESULT(hal_i2c_dev_hw_close(pdev));
    pdev->ctlr = NULL;
    COMPAT_MEM_SET(&pdev->cfg, 0, sizeof(pdev->cfg));
    return VFS_OK;
}

int hal_i2c_dev_hw_open(struct hal_i2c_dev* dev)
{
    hal_i2c_f1_ctx_t* ctx;
    uint32_t i;

    if (!dev || !dev->ctlr || !dev->ctlr->i2c)
        return VFS_ERR_INVAL;
    if (!dev->ctlr->bus_ready)
        return VFS_ERR_NODEV;
    if (dev->hw_open)
        return VFS_OK;
    if (dev->cfg.addr_width != 0)
        return VFS_ERR_NOTSUPP; /* 仅 7-bit */

    ctx = hal_i2c_f1_find((I2C_TypeDef*)dev->ctlr->i2c);
    if (!ctx)
    {
        for (i = 0; i < HAL_I2C_F1_INSTANCES; i++)
        {
            if (!s_ctx[i].used) { ctx = &s_ctx[i]; break; }
        }
        if (!ctx)
            return VFS_ERR_NOMEM;
        COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
        ctx->used            = true;
        ctx->host            = dev->ctlr;
        ctx->hi2c.Instance   = (I2C_TypeDef*)dev->ctlr->i2c;
        ctx->hi2c.State      = HAL_I2C_STATE_RESET;
    }

    /* 参数从结构体读 (F1 用 ClockSpeed, 无 I2C_MODE_I2C) */
    ctx->hi2c.Init.ClockSpeed      = dev->cfg.clock_speed_hz;
    ctx->hi2c.Init.DutyCycle       = dev->cfg.duty_cycle ? dev->cfg.duty_cycle : I2C_DUTYCYCLE_2;
    ctx->hi2c.Init.OwnAddress1     = dev->cfg.own_address;
    ctx->hi2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    ctx->hi2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    ctx->hi2c.Init.OwnAddress2     = 0;
    ctx->hi2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    ctx->hi2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&ctx->hi2c) != HAL_OK)
        return VFS_ERR_IO;

    /* DMA 句柄 (dma_enable=1 时) */
    ctx->dma_tx_ok = false;
    ctx->dma_rx_ok = false;
    if (dev->ctlr->cfg.dma_tx.dma_enable && dev->ctlr->cfg.dma_tx.dma_handle)
    {
        DMA_Channel_TypeDef* ch = hal_i2c_f1_dma_channel(dev->ctlr->cfg.dma_tx.dma_handle,
                                                         dev->ctlr->cfg.dma_tx.dma_channel);
        if (ch)
        {
            ctx->hdmatx.Instance = ch;
            ctx->hdmatx.Init.Direction = DMA_MEMORY_TO_PERIPH;
            ctx->hdmatx.Init.PeriphInc = DMA_PINC_DISABLE;
            ctx->hdmatx.Init.MemInc    = DMA_MINC_ENABLE;
            ctx->hdmatx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
            ctx->hdmatx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
            ctx->hdmatx.Init.Mode = DMA_NORMAL;
            ctx->hdmatx.Init.Priority = dev->ctlr->cfg.dma_tx.dma_priority;
            if (HAL_DMA_Init(&ctx->hdmatx) == HAL_OK)
            {
                __HAL_LINKDMA(&ctx->hi2c, hdmatx, ctx->hdmatx);
                ctx->dma_tx_ok = true;
            }
        }
    }
    if (dev->ctlr->cfg.dma_rx.dma_enable && dev->ctlr->cfg.dma_rx.dma_handle)
    {
        DMA_Channel_TypeDef* ch = hal_i2c_f1_dma_channel(dev->ctlr->cfg.dma_rx.dma_handle,
                                                         dev->ctlr->cfg.dma_rx.dma_channel);
        if (ch)
        {
            ctx->hdmarx.Instance = ch;
            ctx->hdmarx.Init.Direction = DMA_PERIPH_TO_MEMORY;
            ctx->hdmarx.Init.PeriphInc = DMA_PINC_DISABLE;
            ctx->hdmarx.Init.MemInc    = DMA_MINC_ENABLE;
            ctx->hdmarx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
            ctx->hdmarx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
            ctx->hdmarx.Init.Mode = DMA_NORMAL;
            ctx->hdmarx.Init.Priority = dev->ctlr->cfg.dma_rx.dma_priority;
            if (HAL_DMA_Init(&ctx->hdmarx) == HAL_OK)
            {
                __HAL_LINKDMA(&ctx->hi2c, hdmarx, ctx->hdmarx);
                ctx->dma_rx_ok = true;
            }
        }
    }

    dev->ctlr->hw_inited = true;
    dev->ctlr->ref_count++;
    dev->hw_open = 1;
    return VFS_OK;
}

int hal_i2c_dev_hw_close(struct hal_i2c_dev* pdev)
{
    hal_i2c_f1_ctx_t* ctx;
    if (!pdev || !pdev->ctlr || !pdev->ctlr->i2c)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open)
        return VFS_OK;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)pdev->ctlr->i2c);
    if (ctx && pdev->ctlr->ref_count <= 1)
        HAL_I2C_DeInit(&ctx->hi2c);
    if (pdev->ctlr->ref_count > 0)
        pdev->ctlr->ref_count--;
    pdev->ctlr->hw_inited = (pdev->ctlr->ref_count > 0);
    pdev->hw_open = 0;
    return VFS_OK;
}

/** 传输前校验 */
static int hal_i2c_f1_prepare(struct hal_i2c_dev* pdev, size_t len)
{
    if (!pdev || !pdev->ctlr || !pdev->ctlr->i2c)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return VFS_ERR_NODEV;
    if (len == 0 || len > HAL_I2C_MAX_XFER)
        return VFS_ERR_INVAL;
    return VFS_OK;
}

/*============================================================================*/
/*                              同步传输                                       */
/*============================================================================*/

int hal_i2c_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    hal_i2c_f1_ctx_t* ctx;
    int ret;

    if (!tx)
        return VFS_ERR_INVAL;
    ret = hal_i2c_f1_prepare(pdev, len);
    if (ret != VFS_OK)
        return ret;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)pdev->ctlr->i2c);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;
    return hal_i2c_f1_map(HAL_I2C_Master_Transmit(&ctx->hi2c,
                           (uint16_t)((pdev->cfg.address & 0x7FU) << 1),
                           (uint8_t*)tx, (uint16_t)len, timeout_ms));
}

int hal_i2c_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    hal_i2c_f1_ctx_t* ctx;
    int ret;

    if (!rx)
        return VFS_ERR_INVAL;
    ret = hal_i2c_f1_prepare(pdev, len);
    if (ret != VFS_OK)
        return ret;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)pdev->ctlr->i2c);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;
    return hal_i2c_f1_map(HAL_I2C_Master_Receive(&ctx->hi2c,
                            (uint16_t)((pdev->cfg.address & 0x7FU) << 1),
                            rx, (uint16_t)len, timeout_ms));
}

int hal_i2c_sync(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                 uint32_t timeout_ms)
{
    int ret;

    if (!tx && !rx)
        return VFS_ERR_INVAL;
    if (tx && !rx)
        return hal_i2c_write(pdev, tx, len, timeout_ms);
    if (!tx && rx)
        return hal_i2c_read(pdev, rx, len, timeout_ms);

    /* 先写后读: HAL 库无 repeated-start 组合, 分两步 (发 STOP 再 START) */
    ret = hal_i2c_write(pdev, tx, len, timeout_ms);
    if (ret != VFS_OK)
        return ret;
    return hal_i2c_read(pdev, rx, len, timeout_ms);
}

/*============================================================================*/
/*                              DMA 传输                                       */
/*============================================================================*/

int hal_i2c_dma_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    hal_i2c_f1_ctx_t* ctx;
    int ret;

    if (!tx)
        return VFS_ERR_INVAL;
    ret = hal_i2c_f1_prepare(pdev, len);
    if (ret != VFS_OK)
        return ret;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)pdev->ctlr->i2c);
    if (!ctx || !ctx->dma_tx_ok)
        return VFS_ERR_NOTSUPP;
    if (len > 0xFFFFU)
        len = 0xFFFFU;
    return hal_i2c_f1_map(HAL_I2C_Master_Transmit_DMA(&ctx->hi2c,
                           (uint16_t)((pdev->cfg.address & 0x7FU) << 1),
                           (uint8_t*)tx, (uint16_t)len));
}

int hal_i2c_dma_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    hal_i2c_f1_ctx_t* ctx;
    int ret;

    if (!rx)
        return VFS_ERR_INVAL;
    ret = hal_i2c_f1_prepare(pdev, len);
    if (ret != VFS_OK)
        return ret;

    ctx = hal_i2c_f1_find((I2C_TypeDef*)pdev->ctlr->i2c);
    if (!ctx || !ctx->dma_rx_ok)
        return VFS_ERR_NOTSUPP;
    if (len > 0xFFFFU)
        len = 0xFFFFU;
    return hal_i2c_f1_map(HAL_I2C_Master_Receive_DMA(&ctx->hi2c,
                            (uint16_t)((pdev->cfg.address & 0x7FU) << 1),
                            rx, (uint16_t)len));
}

int hal_i2c_dma_write_then_read(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx,
                                size_t len, uint32_t timeout_ms)
{
    /* HAL 库无 repeated-start DMA 组合, 分两步 (发 STOP 再 START) */
    int ret = hal_i2c_dma_write(pdev, tx, len, timeout_ms);
    if (ret != VFS_OK)
        return ret;
    return hal_i2c_dma_read(pdev, rx, len, timeout_ms);
}
