/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPI HAL — STM32F1 实现 (LL 库)
 *
 * 设计: 上层接口 hal_spi.h 不变, 平台实现直接用 STM32 LL 库函数。
 * - 初始化照 stm32_slice 官方生成写法 (LL_SPI_InitTypeDef + LL_GPIO_Init + LL_DMA_SetXxx)。
 * - 时钟: LL_APB2_GRP1_EnableClock / LL_APB1_GRP1_EnableClock。
 * - 同步: LL_SPI_TransmitData8 / LL_SPI_ReceiveData8 (轮询, 全双工)。
 * - 不依赖 STM32 HAL 库 (stm32f1xx_hal_*.c), 仅依赖 LL 层头文件。
 */
#include "hal_spi.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_gpio.h"
#include "stm32f1xx_ll_spi.h"
#include "stm32f1xx_ll_dma.h"

#define HAL_SPI_F1_INSTANCES 1U

typedef struct hal_spi_f1_ctx
{
    bool                   used;
    struct hal_spi_bus_host* host;
    SPI_TypeDef*           spi;      /* SPI 外设基址 */
    DMA_TypeDef*           dma_tx;   /* TX DMA 控制器 */
    DMA_TypeDef*           dma_rx;   /* RX DMA 控制器 */
    uint32_t               dma_tx_ch;
    uint32_t               dma_rx_ch;
    bool                   dma_tx_ok;
    bool                   dma_rx_ok;
} hal_spi_f1_ctx_t;

static hal_spi_f1_ctx_t s_ctx[HAL_SPI_F1_INSTANCES];

static hal_spi_f1_ctx_t* hal_spi_f1_find(SPI_TypeDef* inst)
{
    uint32_t i;
    for (i = 0; i < HAL_SPI_F1_INSTANCES; i++)
        if (s_ctx[i].used && s_ctx[i].spi == inst)
            return &s_ctx[i];
    return NULL;
}

/** 按 GPIO 基址使能端口时钟 */
static void hal_spi_f1_enable_gpio_clock(uintptr_t port)
{
    if (port == GPIOA_BASE)      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    else if (port == GPIOB_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    else if (port == GPIOC_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    else if (port == GPIOD_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOD);
    else if (port == GPIOE_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOE);
}

/** 按 SPI 基址使能外设时钟 (SPI1=APB2, SPI2=APB1; F103xB 无 SPI3) */
static void hal_spi_f1_enable_periph_clock(uintptr_t base)
{
    if (base == SPI1_BASE)      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
    else if (base == SPI2_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
}

/** 配置 SPI 引脚 (MOSI/SCK=复用推挽, MISO=浮空输入) */
static void hal_spi_f1_config_pin(const struct hal_spi_pin_cfg* pin)
{
    LL_GPIO_InitTypeDef gi = {0};
    gi.Pin        = pin->pin;                                   /* LL 位掩码 */
    gi.Mode       = pin->mode ? pin->mode : LL_GPIO_MODE_ALTERNATE;
    gi.Speed      = pin->speed ? pin->speed : LL_GPIO_SPEED_FREQ_HIGH;
    gi.OutputType = pin->output_type ? pin->output_type : LL_GPIO_OUTPUT_PUSHPULL;
    gi.Pull       = pin->pull;
    LL_GPIO_Init((GPIO_TypeDef*)pin->port, &gi);
}

/** dtsi 通道号(1..7) → F1 LL_DMA_CHANNEL_x */
static uint32_t hal_spi_f1_dma_ch(uint32_t channel)
{
    static const uint32_t f1_ch[] =
    {
        LL_DMA_CHANNEL_1, LL_DMA_CHANNEL_2, LL_DMA_CHANNEL_3, LL_DMA_CHANNEL_4,
        LL_DMA_CHANNEL_5, LL_DMA_CHANNEL_6, LL_DMA_CHANNEL_7
    };
    if (channel < 1U || channel > 7U)
        return 0U;
    return f1_ch[channel - 1U];
}

/** 逐字段配置 DMA 通道 (官方 LL_DMA_SetXxx 写法); direction 由 TX/RX 角色决定 */
static bool hal_spi_f1_dma_config(DMA_TypeDef* dma, uint32_t ch, uint32_t direction,
                                  const struct hal_spi_dma_config* cfg)
{
    if (!dma || !ch)
        return false;
    LL_DMA_DeInit(dma, ch);
    LL_DMA_SetDataTransferDirection(dma, ch, direction);
    LL_DMA_SetChannelPriorityLevel(dma, ch, cfg->dma_priority ?
                                            cfg->dma_priority : LL_DMA_PRIORITY_LOW);
    LL_DMA_SetMode(dma, ch, cfg->dma_mode ? cfg->dma_mode : LL_DMA_MODE_NORMAL);
    LL_DMA_SetPeriphIncMode(dma, ch, cfg->dma_periph_inc ?
                                     cfg->dma_periph_inc : LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(dma, ch, cfg->dma_mem_inc ?
                                     cfg->dma_mem_inc : LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(dma, ch, cfg->dma_periph_data_size ?
                                  cfg->dma_periph_data_size : LL_DMA_PDATAALIGN_BYTE);
    LL_DMA_SetMemorySize(dma, ch, cfg->dma_memory_size ?
                                  cfg->dma_memory_size : LL_DMA_MDATAALIGN_BYTE);
    return true;
}

/*============================================================================*/
/*                              Bus / Device 管理                             */
/*============================================================================*/

int hal_spi_bus_host_init(struct hal_spi_bus_host* host, int hw_idx,
                          const struct hal_spi_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= (int)HAL_SPI_HOST_MAX)
        return VFS_ERR_INVAL;
    if (host->bus_ready)
        return VFS_OK;
    if (!cfg->spi)
        return VFS_ERR_NODEV;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;
    if (host->cfg.max_transfer_sz == 0 || host->cfg.max_transfer_sz > HAL_SPI_MAX_XFER)
        host->cfg.max_transfer_sz = HAL_SPI_MAX_XFER;

    /* 外设 + GPIO 时钟 */
    hal_spi_f1_enable_periph_clock(cfg->spi);
    hal_spi_f1_enable_gpio_clock(cfg->mosi.port);
    hal_spi_f1_enable_gpio_clock(cfg->miso.port);
    hal_spi_f1_enable_gpio_clock(cfg->sclk.port);

    /* GPIO: MOSI/SCK 复用推挽, MISO 浮空 (由 dtsi mode 决定) */
    hal_spi_f1_config_pin(&cfg->mosi);
    hal_spi_f1_config_pin(&cfg->miso);
    hal_spi_f1_config_pin(&cfg->sclk);

    /* SPI 初始化 (官方 LL_SPI_InitTypeDef 字段, dtsi 提供 LL 宏值) */
    LL_SPI_InitTypeDef si = {0};
    si.TransferDirection = cfg->bus_role == HAL_SPI_BUS_ROLE_MASTER ?
                           LL_SPI_FULL_DUPLEX : LL_SPI_HALF_DUPLEX_RX;
    si.Mode              = cfg->bus_role == HAL_SPI_BUS_ROLE_MASTER ?
                           LL_SPI_MODE_MASTER : LL_SPI_MODE_SLAVE;
    si.DataWidth         = LL_SPI_DATAWIDTH_8BIT;
    si.ClockPolarity     = LL_SPI_POLARITY_LOW;
    si.ClockPhase        = LL_SPI_PHASE_1EDGE;
    si.NSS               = LL_SPI_NSS_SOFT;
    si.BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV4;
    si.BitOrder          = LL_SPI_MSB_FIRST;
    si.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    si.CRCPoly           = 10U;
    LL_SPI_Init((SPI_TypeDef*)cfg->spi, &si);

    /* DMA 时钟预使能 (通道级配置在 hal_spi_dev_hw_open 完成) */
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

    host->spi       = cfg->spi;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return VFS_OK;
}

int hal_spi_bus_host_deinit(struct hal_spi_bus_host* host)
{
    hal_spi_f1_ctx_t* ctx;
    if (!host)
        return VFS_ERR_INVAL;
    if (!host->bus_ready)
        return VFS_OK;

    ctx = hal_spi_f1_find((SPI_TypeDef*)host->spi);
    if (ctx)
    {
        LL_SPI_Disable(ctx->spi);
        ctx->used = false;
    }
    host->bus_ready = false;
    host->hw_inited = false;
    return VFS_OK;
}

int hal_spi_dev_init(struct hal_spi_dev* pdev, struct hal_spi_bus_host* host,
                     const struct hal_spi_device_config* dev_cfg)
{
    if (!pdev || !host || !dev_cfg)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->ctlr = host;
    pdev->cfg  = *dev_cfg;
    return VFS_OK;
}

int hal_spi_dev_hw_open(struct hal_spi_dev* pdev)
{
    hal_spi_f1_ctx_t* ctx;
    uint32_t i;

    if (!pdev || !pdev->ctlr || !pdev->ctlr->spi)
        return VFS_ERR_INVAL;
    if (!pdev->ctlr->bus_ready)
        return VFS_ERR_NODEV;
    if (pdev->hw_open)
        return VFS_OK;

    ctx = hal_spi_f1_find((SPI_TypeDef*)pdev->ctlr->spi);
    if (!ctx)
    {
        for (i = 0; i < HAL_SPI_F1_INSTANCES; i++)
        {
            if (!s_ctx[i].used) { ctx = &s_ctx[i]; break; }
        }
        if (!ctx)
            return VFS_ERR_NOMEM;
        COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
        ctx->used = true;
        ctx->host = pdev->ctlr;
        ctx->spi  = (SPI_TypeDef*)pdev->ctlr->spi;
    }

    /* 按设备配置重配 SPI (时钟极性/相位/位序/分频) */
    LL_SPI_InitTypeDef si = {0};
    si.TransferDirection = pdev->cfg.transfer_direction ? pdev->cfg.transfer_direction : LL_SPI_FULL_DUPLEX;
    si.Mode              = LL_SPI_MODE_MASTER;
    si.DataWidth         = pdev->cfg.data_width ? pdev->cfg.data_width : LL_SPI_DATAWIDTH_8BIT;
    si.ClockPolarity     = (pdev->cfg.mode & 0x02) ? LL_SPI_POLARITY_HIGH : LL_SPI_POLARITY_LOW;
    si.ClockPhase        = (pdev->cfg.mode & 0x01) ? LL_SPI_PHASE_2EDGE : LL_SPI_PHASE_1EDGE;
    si.NSS               = pdev->cfg.nss ? pdev->cfg.nss : LL_SPI_NSS_SOFT;
    si.BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV16;
    si.BitOrder          = pdev->cfg.bit_order ? pdev->cfg.bit_order : LL_SPI_MSB_FIRST;
    si.CRCCalculation    = pdev->cfg.crc_calculation ? pdev->cfg.crc_calculation : LL_SPI_CRCCALCULATION_DISABLE;
    si.CRCPoly           = pdev->cfg.crc_poly ? pdev->cfg.crc_poly : 7U;
    LL_SPI_Init(ctx->spi, &si);

    /* DMA (dma_enable=1 时) */
    ctx->dma_tx_ok = false;
    ctx->dma_rx_ok = false;
    if (pdev->ctlr->cfg.dma_tx.dma_enable && pdev->ctlr->cfg.dma_tx.dma_handle)
    {
        ctx->dma_tx = (DMA_TypeDef*)pdev->ctlr->cfg.dma_tx.dma_handle;
        ctx->dma_tx_ch = hal_spi_f1_dma_ch(pdev->ctlr->cfg.dma_tx.dma_channel);
        if (hal_spi_f1_dma_config(ctx->dma_tx, ctx->dma_tx_ch,
                                  LL_DMA_DIRECTION_MEMORY_TO_PERIPH, &pdev->ctlr->cfg.dma_tx))
            ctx->dma_tx_ok = true;
    }
    if (pdev->ctlr->cfg.dma_rx.dma_enable && pdev->ctlr->cfg.dma_rx.dma_handle)
    {
        ctx->dma_rx = (DMA_TypeDef*)pdev->ctlr->cfg.dma_rx.dma_handle;
        ctx->dma_rx_ch = hal_spi_f1_dma_ch(pdev->ctlr->cfg.dma_rx.dma_channel);
        if (hal_spi_f1_dma_config(ctx->dma_rx, ctx->dma_rx_ch,
                                  LL_DMA_DIRECTION_PERIPH_TO_MEMORY, &pdev->ctlr->cfg.dma_rx))
            ctx->dma_rx_ok = true;
    }

    LL_SPI_Enable(ctx->spi);
    pdev->ctlr->hw_inited = true;
    pdev->ctlr->ref_count++;
    pdev->hw_open = 1;
    return VFS_OK;
}

int hal_spi_dev_hw_close(struct hal_spi_dev* pdev)
{
    hal_spi_f1_ctx_t* ctx;
    if (!pdev || !pdev->ctlr || !pdev->ctlr->spi)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open)
        return VFS_OK;

    ctx = hal_spi_f1_find((SPI_TypeDef*)pdev->ctlr->spi);
    if (ctx && pdev->ctlr->ref_count <= 1)
        LL_SPI_Disable(ctx->spi);
    if (pdev->ctlr->ref_count > 0)
        pdev->ctlr->ref_count--;
    pdev->ctlr->hw_inited = (pdev->ctlr->ref_count > 0);
    pdev->hw_open = 0;
    return VFS_OK;
}

/*============================================================================*/
/*                              同步传输 (master)                             */
/*============================================================================*/

static int hal_spi_f1_cs(struct hal_spi_dev* pdev, int level)
{
    if (pdev->cfg.cs_pin < 0 || !pdev->cfg.cs_port)
        return VFS_OK; /* 无硬件 CS */
    if (level)
        LL_GPIO_SetOutputPin((GPIO_TypeDef*)pdev->cfg.cs_port, (uint32_t)pdev->cfg.cs_pin);
    else
        LL_GPIO_ResetOutputPin((GPIO_TypeDef*)pdev->cfg.cs_port, (uint32_t)pdev->cfg.cs_pin);
    return VFS_OK;
}

/** 传输前校验 + CS 拉低 */
static int hal_spi_f1_prepare(struct hal_spi_dev* pdev, size_t len)
{
    if (!pdev || !pdev->ctlr || !pdev->ctlr->spi)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return VFS_ERR_NODEV;
    if (len == 0 || len > HAL_SPI_MAX_XFER)
        return VFS_ERR_INVAL;
    hal_spi_f1_cs(pdev, 0);
    return VFS_OK;
}

/** LL 轮询全双工/单工传输 (F1) */
static int hal_spi_f1_xfer_poll(SPI_TypeDef* spi, const uint8_t* tx, uint8_t* rx,
                                size_t len, uint32_t guard)
{
    size_t i;
    for (i = 0; i < len; i++)
    {
        uint32_t g = 0;
        /* 等 TXE 空闲 (丢弃上一字节残留 RX) */
        while (!LL_SPI_IsActiveFlag_TXE(spi))
        {
            if (++g > guard) return VFS_ERR_TIMEOUT;
        }
        LL_SPI_TransmitData8(spi, tx ? tx[i] : 0xFFU);
        /* 等 RXNE 就绪后读回 (全双工) */
        g = 0;
        while (!LL_SPI_IsActiveFlag_RXNE(spi))
        {
            if (++g > guard) return VFS_ERR_TIMEOUT;
        }
        if (rx)
            rx[i] = (uint8_t)LL_SPI_ReceiveData8(spi);
        else
            (void)LL_SPI_ReceiveData8(spi);
    }
    /* 等待 BUSY 释放 */
    uint32_t g = 0;
    while (LL_SPI_IsActiveFlag_BSY(spi))
    {
        if (++g > guard) return VFS_ERR_TIMEOUT;
    }
    return VFS_OK;
}

int hal_spi_sync(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                 uint32_t timeout_ms, uint32_t xfer_mode)
{
    hal_spi_f1_ctx_t* ctx;
    int ret;
    uint32_t guard = (timeout_ms ? timeout_ms : 100U) * 4000U;

    if (!tx && !rx)
        return VFS_ERR_INVAL;
    ret = hal_spi_f1_prepare(pdev, len);
    if (ret != VFS_OK)
        return ret;

    ctx = hal_spi_f1_find((SPI_TypeDef*)pdev->ctlr->spi);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;

    /* xfer_mode: DMA 且可用走 DMA, 否则轮询 (DMA 全双工 F1 LL 未实现, 走轮询) */
    if (xfer_mode == HAL_SPI_XFER_DMA && ctx->dma_tx_ok && ctx->dma_rx_ok)
        ret = VFS_ERR_NOTSUPP; /* F1 LL 未实现 SPI 全双工 DMA, 返回 NOTSUPP 由上层退化 */
    else
        ret = hal_spi_f1_xfer_poll(ctx->spi, tx, rx, len, guard);

    hal_spi_f1_cs(pdev, 1);
    return ret;
}

int hal_spi_transfer_async(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                           hal_spi_callback_t cb, void* userdata)
{
    /* STM32 不支持异步, 返回 NOTSUPP; 调用方走 sync */
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(tx);
    COMPAT_UNUSED_PARAM(rx);
    COMPAT_UNUSED_PARAM(len);
    COMPAT_UNUSED_PARAM(cb);
    COMPAT_UNUSED_PARAM(userdata);
    return VFS_ERR_NOTSUPP;
}

int hal_spi_transfer_poll(struct hal_spi_dev* pdev, uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return VFS_ERR_NOTSUPP;
}

int hal_spi_get_trans_result(struct hal_spi_dev* pdev, uint8_t* rx_data, size_t rx_cap,
                             size_t* trans_len, uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(rx_data);
    COMPAT_UNUSED_PARAM(rx_cap);
    COMPAT_UNUSED_PARAM(trans_len);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return VFS_ERR_NOTSUPP;
}

int hal_spi_slave_sync(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                       uint32_t timeout_ms)
{
    hal_spi_f1_ctx_t* ctx;
    uint32_t guard = (timeout_ms ? timeout_ms : 100U) * 4000U;

    if (!pdev || !pdev->ctlr || !pdev->ctlr->spi)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open)
        return VFS_ERR_NODEV;
    if (len == 0 || len > HAL_SPI_MAX_XFER)
        return VFS_ERR_INVAL;

    ctx = hal_spi_f1_find((SPI_TypeDef*)pdev->ctlr->spi);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;

    return hal_spi_f1_xfer_poll(ctx->spi, tx, rx, len, guard);
}

int hal_spi_slave_queue_tx(struct hal_spi_dev* pdev, const uint8_t* data, size_t len,
                           uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(data);
    COMPAT_UNUSED_PARAM(len);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return VFS_ERR_NOTSUPP;
}
