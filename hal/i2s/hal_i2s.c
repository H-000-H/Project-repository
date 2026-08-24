/* SPDX-License-Identifier: Apache-2.0 */
/*
 * I2S HAL — STM32F4 (SPI I2S 模式)
 * sync: poll / DMA NORMAL(TC 轮询) / AUTO; circular+fifo_spsc;
 * HT/TC/async: ioctl 配置; 虚拟中断上/下半部注释空实现占位
 */
#include "hal_i2s.h"
#include "buffer.h"
#include "compiler_compat.h"
#include "interrupt.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_spi.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_hal.h"

/* forward */
int hal_i2s_dma_circ_stop(struct hal_i2s_dev* pdev);

#define I2S_OR_DEF(v, d) ((v) ? (v) : (d))

/* I2S 下半部工作槽由中间件 interrupt.c 提供（interrupt.h extern）；本文件仅绑定 fn/arg */

/* per-host dummy: TX-only / RX-only 时防 OVR / 驱动时钟 */
static uint16_t s_dummy_tx[HAL_I2S_HOST_MAX][HAL_I2S_MAX_XFER] COMPAT_ALIGNED(32);
static uint16_t s_dummy_rx[HAL_I2S_HOST_MAX][HAL_I2S_MAX_XFER] COMPAT_ALIGNED(32);

/**
 * @brief 配置 I2S 引脚为复用功能
 * @param g 引脚配置指针, 为 NULL 或 port/pin 为空时跳过
 * @return 成功返回 MINI_OK
 */
static int i2s_gpio_config(const struct hal_i2s_pin_cfg* g)
{
    LL_GPIO_InitTypeDef init = {0};
    if (!g || !g->port || !g->pin)
        return MINI_OK;
    init.Pin = g->pin;
    init.Mode = g->mode ? g->mode : LL_GPIO_MODE_ALTERNATE;
    init.Speed = g->speed ? g->speed : LL_GPIO_SPEED_FREQ_HIGH;
    init.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    init.Pull = g->pull ? g->pull : LL_GPIO_PULL_NO;
    init.Alternate = g->af;
    if (g->clk_bus)
        LL_AHB1_GRP1_EnableClock(g->clk_bus);
    LL_GPIO_Init((GPIO_TypeDef*)g->port, &init);
    return MINI_OK;
}

/**
 * @brief 使能 DMA 控制器时钟
 * @param dma_base DMA 控制器基址 (DMA1_BASE 或 DMA2_BASE)
 */
static void i2s_enable_dma_clock(uintptr_t dma_base)
{
    uint32_t periph = (dma_base == DMA1_BASE) ? LL_AHB1_GRP1_PERIPH_DMA1
                      : (dma_base == DMA2_BASE) ? LL_AHB1_GRP1_PERIPH_DMA2 : 0U;
    if (periph)
        LL_AHB1_GRP1_EnableClock(periph);
}

/**
 * @brief 清除指定 DMA 流的传输完成 (TC) 标志
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号 (0..7)
 */
static void i2s_dma_clear_tc(DMA_TypeDef* dma, uint32_t stream)
{
    static void (*const clear_tc[])(DMA_TypeDef*) = {
        LL_DMA_ClearFlag_TC0, LL_DMA_ClearFlag_TC1, LL_DMA_ClearFlag_TC2, LL_DMA_ClearFlag_TC3,
        LL_DMA_ClearFlag_TC4, LL_DMA_ClearFlag_TC5, LL_DMA_ClearFlag_TC6, LL_DMA_ClearFlag_TC7
    };
    if (stream <= LL_DMA_STREAM_7)
        clear_tc[stream](dma);
}

/**
 * @brief 清除指定 DMA 流的半传输 (HT) 标志
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号 (0..7)
 */
static void i2s_dma_clear_ht(DMA_TypeDef* dma, uint32_t stream)
{
    static void (*const clear_ht[])(DMA_TypeDef*) = {
        LL_DMA_ClearFlag_HT0, LL_DMA_ClearFlag_HT1, LL_DMA_ClearFlag_HT2, LL_DMA_ClearFlag_HT3,
        LL_DMA_ClearFlag_HT4, LL_DMA_ClearFlag_HT5, LL_DMA_ClearFlag_HT6, LL_DMA_ClearFlag_HT7
    };
    if (stream <= LL_DMA_STREAM_7)
        clear_ht[stream](dma);
}

/**
 * @brief 按 irq_mode 使能/关闭 DMA HT/TC 中断 (circular / async 共用)
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号
 * @param irq_mode 中断模式 (HAL_I2S_IRQ_HT / TC / HT_TC)
 */
static void i2s_dma_apply_irq_mode(DMA_TypeDef* dma, uint32_t stream, uint32_t irq_mode)
{
    if (!dma)
        return;
    LL_DMA_DisableIT_HT(dma, stream);
    LL_DMA_DisableIT_TC(dma, stream);
    if (irq_mode == HAL_I2S_IRQ_HT || irq_mode == HAL_I2S_IRQ_HT_TC)
        LL_DMA_EnableIT_HT(dma, stream);
    if (irq_mode == HAL_I2S_IRQ_TC || irq_mode == HAL_I2S_IRQ_HT_TC)
        LL_DMA_EnableIT_TC(dma, stream);
}

/**
 * @brief 使能 IT 前绑定全局 I2S 下半部 work (fn/arg/原子位)
 * @param pdev I2S 设备指针
 */
static void i2s_bind_bottom_half(struct hal_i2s_dev* pdev)
{
    g_i2s_bottom_half_work.fn  = hal_i2s_dma_bottom_half_handler;
    g_i2s_bottom_half_work.arg = pdev;
    COMPAT_ATOMIC_STORE(&g_i2s_bottom_half_work.pending,   false, COMPAT_MO_SEQ_CST);
    COMPAT_ATOMIC_STORE(&g_i2s_bottom_half_work.executing, false, COMPAT_MO_SEQ_CST);
    COMPAT_ATOMIC_STORE(&g_i2s_bottom_half_work.rerun,     false, COMPAT_MO_SEQ_CST);
}


/**
 * @brief 检查指定 DMA 流是否传输完成
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号 (0..7)
 * @return true 表示 TC 标志已置位, false 表示未完成或 stream 非法
 */
static bool i2s_dma_is_tc(DMA_TypeDef* dma, uint32_t stream)
{
    switch (stream)
    {
    case LL_DMA_STREAM_0: return LL_DMA_IsActiveFlag_TC0(dma);
    case LL_DMA_STREAM_1: return LL_DMA_IsActiveFlag_TC1(dma);
    case LL_DMA_STREAM_2: return LL_DMA_IsActiveFlag_TC2(dma);
    case LL_DMA_STREAM_3: return LL_DMA_IsActiveFlag_TC3(dma);
    case LL_DMA_STREAM_4: return LL_DMA_IsActiveFlag_TC4(dma);
    case LL_DMA_STREAM_5: return LL_DMA_IsActiveFlag_TC5(dma);
    case LL_DMA_STREAM_6: return LL_DMA_IsActiveFlag_TC6(dma);
    case LL_DMA_STREAM_7: return LL_DMA_IsActiveFlag_TC7(dma);
    default: return false;
    }
}

/**
 * @brief 按配置静态初始化 I2S DMA 流
 * @param cfg DMA 配置指针, 为 NULL 或未使能时跳过
 * @param direction DMA 传输方向 (LL_DMA_DIRECTION_*)
 */
static void i2s_dma_init_static(const struct hal_i2s_dma_config* cfg, uint32_t direction)
{
    LL_DMA_InitTypeDef init = {0};
    if (!cfg || !cfg->dma_handle || !cfg->dma_enable)
        return;
    i2s_enable_dma_clock(cfg->dma_handle);
    LL_DMA_DisableStream((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream);
    init.PeriphOrM2MSrcAddress  = 0;
    init.MemoryOrM2MDstAddress  = 0;
    init.Direction              = direction;
    init.Mode                   = cfg->dma_mode; /* 0=NORMAL */
    init.PeriphOrM2MSrcIncMode  = cfg->dma_periph_inc;
    init.MemoryOrM2MDstIncMode  = I2S_OR_DEF(cfg->dma_mem_inc, LL_DMA_MEMORY_INCREMENT);
    init.PeriphOrM2MSrcDataSize = I2S_OR_DEF(cfg->dma_periph_data_size, LL_DMA_PDATAALIGN_HALFWORD);
    init.MemoryOrM2MDstDataSize = I2S_OR_DEF(cfg->dma_memory_size, LL_DMA_MDATAALIGN_HALFWORD);
    init.NbData                 = 0;
    init.Channel                = cfg->dma_channel;
    init.Priority               = cfg->dma_priority;
    init.FIFOMode               = cfg->dma_fifo_mode;
    init.FIFOThreshold          = cfg->dma_fifo_threshold;
    init.MemBurst               = cfg->dma_mem_burst;
    init.PeriphBurst            = cfg->dma_periph_burst;
    LL_DMA_Init((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream, &init);
}

/**
 * @brief 轮询等待 SPI/I2S 忙标志清零
 * @param spi SPI/I2S 外设指针
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int i2s_wait_bsy(SPI_TypeDef* spi, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (LL_SPI_IsActiveFlag_BSY(spi))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }
    return MINI_OK;
}

/**
 * @brief 通过 DMA 同步传输 I2S 采样数据
 * @param host I2S host 指针
 * @param tx 发送采样缓冲 (可为 NULL, TX-only 时用 dummy)
 * @param rx 接收采样缓冲 (可为 NULL, TX-only 时丢弃)
 * @param samples 采样点数 (16-bit)
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 * @note 支持 TX-only / RX-only / 全双工, 缺缓冲时使用 dummy 缓冲
 */
static int i2s_transfer_dma(struct hal_i2s_bus_host* host, const uint16_t* tx, uint16_t* rx,
                            size_t samples, uint32_t timeout_ms)
{
    SPI_TypeDef* spi;
    DMA_TypeDef* dma_tx;
    DMA_TypeDef* dma_rx;
    uint32_t tx_stream, rx_stream, start;
    int use_tx, use_rx;
    const uint16_t* tx_buf = tx;
    uint16_t* rx_buf = rx;

    if (!host || samples == 0 || (!tx && !rx))
        return MINI_ERR_INVAL;
    if (samples > HAL_I2S_MAX_XFER)
        return MINI_ERR_INVAL;
    spi = (SPI_TypeDef*)host->spi;
    if (!spi)
        return MINI_ERR_IO;

    /* 按调用方缓冲决定方向 (TX-only / RX-only / 全双工) */
    use_tx = (tx != NULL);
    use_rx = (rx != NULL);

    if ((use_tx && !host->cfg.dma_tx.dma_enable) || (use_rx && !host->cfg.dma_rx.dma_enable))
        return MINI_ERR_NOTSUPP;

    dma_tx = (DMA_TypeDef*)host->cfg.dma_tx.dma_handle;
    dma_rx = (DMA_TypeDef*)host->cfg.dma_rx.dma_handle;
    tx_stream = host->cfg.dma_tx.dma_stream;
    rx_stream = host->cfg.dma_rx.dma_stream;
    if ((use_tx && !dma_tx) || (use_rx && !dma_rx))
        return MINI_ERR_IO;

    if (use_tx && !tx_buf)
    {
        COMPAT_MEM_SET(s_dummy_tx[host->hw_idx], 0, samples * sizeof(uint16_t));
        tx_buf = s_dummy_tx[host->hw_idx];
    }
    if (use_rx && !rx_buf)
        rx_buf = s_dummy_rx[host->hw_idx];

    if (use_tx)
    {
        LL_DMA_DisableStream(dma_tx, tx_stream);
        LL_DMA_ConfigAddresses(dma_tx, tx_stream, (uint32_t)tx_buf, (uint32_t)&spi->DR,
                               LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
        LL_DMA_SetDataLength(dma_tx, tx_stream, (uint32_t)samples);
        i2s_dma_clear_tc(dma_tx, tx_stream);
        LL_SPI_EnableDMAReq_TX(spi);
    }
    if (use_rx)
    {
        LL_DMA_DisableStream(dma_rx, rx_stream);
        LL_DMA_ConfigAddresses(dma_rx, rx_stream, (uint32_t)&spi->DR, (uint32_t)rx_buf,
                               LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
        LL_DMA_SetDataLength(dma_rx, rx_stream, (uint32_t)samples);
        i2s_dma_clear_tc(dma_rx, rx_stream);
        LL_SPI_EnableDMAReq_RX(spi);
    }
    if (use_rx)
        LL_DMA_EnableStream(dma_rx, rx_stream);
    if (use_tx)
        LL_DMA_EnableStream(dma_tx, tx_stream);

    start = HAL_GetTick();
    while ((use_tx && !i2s_dma_is_tc(dma_tx, tx_stream)) ||
           (use_rx && !i2s_dma_is_tc(dma_rx, rx_stream)))
           {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            goto timeout;
    }
    if (i2s_wait_bsy(spi, timeout_ms) != MINI_OK)
        goto timeout;

    if (use_tx)
    {
        i2s_dma_clear_tc(dma_tx, tx_stream);
        LL_DMA_DisableStream(dma_tx, tx_stream);
        LL_SPI_DisableDMAReq_TX(spi);
    }
    if (use_rx)
    {
        i2s_dma_clear_tc(dma_rx, rx_stream);
        LL_DMA_DisableStream(dma_rx, rx_stream);
        LL_SPI_DisableDMAReq_RX(spi);
    }
    return MINI_OK;

timeout:
    if (use_tx)
    {
        LL_DMA_DisableStream(dma_tx, tx_stream);
        LL_SPI_DisableDMAReq_TX(spi);
    }
    if (use_rx)
    {
        LL_DMA_DisableStream(dma_rx, rx_stream);
        LL_SPI_DisableDMAReq_RX(spi);
    }
    return MINI_ERR_TIMEOUT;
}

/**
 * @brief 轮询方式逐采样传输 I2S 数据
 * @param pdev I2S 设备指针
 * @param tx 发送采样缓冲 (可为 NULL)
 * @param rx 接收采样缓冲 (可为 NULL)
 * @param samples 采样点数 (16-bit)
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int i2s_transfer_poll(struct hal_i2s_dev* pdev, const uint16_t* tx, uint16_t* rx,
                             size_t samples, uint32_t timeout_ms)
{
    SPI_TypeDef* spi = (SPI_TypeDef*)pdev->ctlr->spi;
    size_t i;
    uint32_t start = HAL_GetTick();

    for (i = 0; i < samples; i++)
    {
        if (tx)
        {
            while (!LL_SPI_IsActiveFlag_TXE(spi))
            {
                if ((HAL_GetTick() - start) >= timeout_ms)
                    return MINI_ERR_TIMEOUT;
            }
            LL_SPI_TransmitData16(spi, tx[i]);
        }
        if (rx)
        {
            while (!LL_SPI_IsActiveFlag_RXNE(spi))
            {
                if ((HAL_GetTick() - start) >= timeout_ms)
                    return MINI_ERR_TIMEOUT;
            }
            rx[i] = LL_SPI_ReceiveData16(spi);
        } else if (tx)
        {
            if (LL_SPI_IsActiveFlag_RXNE(spi))
                (void)LL_SPI_ReceiveData16(spi);
        }
    }
    return MINI_OK;
}

/**
 * @brief 初始化 I2S 总线主机 (时钟/GPIO/DMA 静态配置)
 * @param host 总线主机对象指针
 * @param hw_idx dummy 缓冲 / HW slot 索引
 * @param cfg 总线配置 (硬件直投)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_NODEV
 */
int hal_i2s_bus_host_init(struct hal_i2s_bus_host* host, int hw_idx, const struct hal_i2s_bus_config* cfg)
{
    if (!host || !cfg || !cfg->spi || hw_idx < 0 || hw_idx >= HAL_I2S_HOST_MAX)
        return MINI_ERR_INVAL;
    if (host->bus_ready)
        return MINI_OK;
    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;
    if (host->cfg.max_transfer_sz == 0 || host->cfg.max_transfer_sz > HAL_I2S_MAX_XFER)
        host->cfg.max_transfer_sz = HAL_I2S_MAX_XFER;

    if (cfg->spi == SPI1_BASE)
        LL_APB2_GRP1_EnableClock(cfg->spi_clk_periph);
    else
        LL_APB1_GRP1_EnableClock(cfg->spi_clk_periph);
    (void)i2s_gpio_config(&cfg->ws);
    (void)i2s_gpio_config(&cfg->ck);
    (void)i2s_gpio_config(&cfg->sd);
    (void)i2s_gpio_config(&cfg->mck);

    if (cfg->dma_tx.dma_enable)
        i2s_dma_init_static(&cfg->dma_tx, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    if (cfg->dma_rx.dma_enable)
        i2s_dma_init_static(&cfg->dma_rx, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

    host->spi = cfg->spi;
    host->hw_idx = hw_idx;
    host->bus_ready = true;
    host->circ_running = 0;
    host->irq_mode = HAL_I2S_IRQ_NONE;
    return MINI_OK;
}

/**
 * @brief 释放 I2S 总线主机 (停 circular DMA、关 I2S、复位状态)
 * @param host 总线主机对象指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2s_bus_host_deinit(struct hal_i2s_bus_host* host)
{
    if (!host) return MINI_ERR_INVAL;
    if (!host->bus_ready) return MINI_OK;
    if (host->circ_running)
    {
        SPI_TypeDef* spi = (SPI_TypeDef*)host->spi;
        if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
        {
            LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
            if (spi) LL_SPI_DisableDMAReq_TX(spi);
        }
        if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
        {
            LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
            if (spi) LL_SPI_DisableDMAReq_RX(spi);
        }
        host->circ_running = 0;
    }
    if (host->spi)
        LL_I2S_Disable((SPI_TypeDef*)host->spi);
    if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
    if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
    host->bus_ready = false;
    host->hw_inited = false;
    host->circ_running = 0;
    return MINI_OK;
}

/**
 * @brief 初始化 I2S 设备对象 (绑定 host 与设备配置)
 * @param pdev I2S 设备指针
 * @param host 总线主机对象指针
 * @param cfg 设备配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_i2s_dev_init(struct hal_i2s_dev* pdev, struct hal_i2s_bus_host* host, const struct hal_i2s_device_config* cfg)
{
    if (!pdev || !host || !cfg) return MINI_ERR_INVAL;
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->ctlr = host;
    pdev->cfg = *cfg;
    return MINI_OK;
}

/**
 * @brief 释放 I2S 设备 (若已打开则先 hw_close)
 * @param pdev I2S 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2s_dev_deinit(struct hal_i2s_dev* pdev)
{
    if (!pdev) return MINI_ERR_INVAL;
    if (pdev->hw_open)
        (void)hal_i2s_dev_hw_close(pdev);
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    return MINI_OK;
}

/**
 * @brief 打开 I2S 设备 (LL_I2S_Init + 使能, 递增 host 引用计数)
 * @param pdev I2S 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL / MINI_ERR_NODEV / MINI_ERR_IO
 */
int hal_i2s_dev_hw_open(struct hal_i2s_dev* pdev)
{
    LL_I2S_InitTypeDef init = {0};
    SPI_TypeDef* spi;

    if (!pdev || !pdev->ctlr || !pdev->ctlr->spi) return MINI_ERR_INVAL;
    if (!pdev->ctlr->bus_ready) return MINI_ERR_NODEV;
    if (pdev->hw_open) return MINI_OK;

    spi = (SPI_TypeDef*)pdev->ctlr->spi;
    LL_I2S_Disable(spi);
    init.Mode = pdev->cfg.mode ? pdev->cfg.mode : LL_I2S_MODE_MASTER_TX;
    init.Standard = pdev->cfg.standard ? pdev->cfg.standard : LL_I2S_STANDARD_PHILIPS;
    init.DataFormat = pdev->cfg.data_format ? pdev->cfg.data_format : LL_I2S_DATAFORMAT_16B;
    init.MCLKOutput = pdev->cfg.mclk_output ? LL_I2S_MCLK_OUTPUT_ENABLE : LL_I2S_MCLK_OUTPUT_DISABLE;
    init.AudioFreq = pdev->cfg.audio_freq ? pdev->cfg.audio_freq : LL_I2S_AUDIOFREQ_48K;
    init.ClockPolarity = pdev->cfg.cpollarity ? pdev->cfg.cpollarity : LL_I2S_POLARITY_LOW;
    if (LL_I2S_Init(spi, &init) != SUCCESS)
        return MINI_ERR_IO;
    LL_I2S_Enable(spi);
    pdev->ctlr->active_cfg = pdev->cfg;
    pdev->ctlr->hw_inited = true;
    pdev->ctlr->ref_count++;
    pdev->hw_open = 1;
    return MINI_OK;
}

/**
 * @brief 关闭 I2S 设备 (停 circular、递减引用, ref=0 时关 I2S)
 * @param pdev I2S 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2s_dev_hw_close(struct hal_i2s_dev* pdev)
{
    if (!pdev || !pdev->ctlr) return MINI_ERR_INVAL;
    if (!pdev->hw_open) return MINI_OK;
    if (pdev->ctlr->circ_running)
        (void)hal_i2s_dma_circ_stop(pdev);
    if (pdev->ctlr->ref_count > 0)
        pdev->ctlr->ref_count--;
    if (pdev->ctlr->ref_count == 0)
    {
        LL_I2S_Disable((SPI_TypeDef*)pdev->ctlr->spi);
        pdev->ctlr->hw_inited = false;
    }
    pdev->hw_open = 0;
    return MINI_OK;
}

/**
 * @brief I2S 同步传输 (poll / DMA / AUTO 路径选择)
 * @param pdev I2S 设备指针
 * @param tx 发送采样缓冲 (可为 NULL)
 * @param rx 接收采样缓冲 (可为 NULL)
 * @param samples 采样点数 (16-bit)
 * @param timeout_ms 超时 (ms)
 * @param xfer_mode HAL_I2S_XFER_POLL / DMA / AUTO
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_* (含 BUSY/NOTSUPP/TIMEOUT)
 */
int hal_i2s_sync(struct hal_i2s_dev* pdev, const uint16_t* tx, uint16_t* rx, size_t samples,
                 uint32_t timeout_ms, uint32_t xfer_mode)
{
    int dma_ok;

    if (!pdev || !pdev->hw_open || samples == 0 || (!tx && !rx))
        return MINI_ERR_INVAL;
    if (samples > pdev->ctlr->cfg.max_transfer_sz)
        return MINI_ERR_INVAL;
    if (pdev->ctlr->circ_running)
        return MINI_ERR_BUSY;

    dma_ok = ((tx && pdev->ctlr->cfg.dma_tx.dma_enable) || !tx) &&
             ((rx && pdev->ctlr->cfg.dma_rx.dma_enable) || !rx);
    /* TX-only 只需 dma_tx; RX-only 只需 dma_rx */
    if (tx && !rx)
        dma_ok = pdev->ctlr->cfg.dma_tx.dma_enable;
    else if (rx && !tx)
        dma_ok = pdev->ctlr->cfg.dma_rx.dma_enable;
    else if (tx && rx)
        dma_ok = pdev->ctlr->cfg.dma_tx.dma_enable && pdev->ctlr->cfg.dma_rx.dma_enable;

    if (xfer_mode == HAL_I2S_XFER_POLL)
        return i2s_transfer_poll(pdev, tx, rx, samples, timeout_ms);
    if (xfer_mode == HAL_I2S_XFER_DMA)
    {
        if (!dma_ok)
            return MINI_ERR_NOTSUPP;
        return i2s_transfer_dma(pdev->ctlr, tx, rx, samples, timeout_ms);
    }
    /* AUTO */
    if (dma_ok)
        return i2s_transfer_dma(pdev->ctlr, tx, rx, samples, timeout_ms);
    return i2s_transfer_poll(pdev, tx, rx, samples, timeout_ms);
}

/**
 * @brief 启动 I2S circular DMA (绑定 circ_fifo, 可选 HT/TC 中断)
 * @param pdev I2S 设备指针
 * @param tx_enable 非 0 使能 TX circular DMA
 * @param rx_enable 非 0 使能 RX circular DMA
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL / MINI_ERR_NOTSUPP
 */
int hal_i2s_dma_circ_start(struct hal_i2s_dev* pdev, int tx_enable, int rx_enable)
{
    struct hal_i2s_bus_host* host;
    SPI_TypeDef* spi;
    struct fifo_spsc* fifo;
    uint32_t nb;

    if (!pdev || !pdev->hw_open || !pdev->ctlr)
        return MINI_ERR_INVAL;
    host = pdev->ctlr;
    fifo = host->cfg.circ_fifo;
    if (!fifo || !fifo->buf || fifo->size == 0)
        return MINI_ERR_INVAL;
    if (!tx_enable && !rx_enable)
        return MINI_ERR_INVAL;
    if (host->circ_running)
        return MINI_OK;

    spi = (SPI_TypeDef*)host->spi;
    nb = fifo->size; /* 元素个数 = 采样数 (fifo_data_type 存 16-bit 采样) */

    if (tx_enable)
    {
        if (!host->cfg.dma_tx.dma_enable || !host->cfg.dma_tx.dma_handle)
            return MINI_ERR_NOTSUPP;
        /* 强制按 circular 运行 (DTS 可写 CIRCULAR; 此处兜底置位) */
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
        LL_DMA_SetMode((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream, LL_DMA_MODE_CIRCULAR);
        LL_DMA_ConfigAddresses((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream,
                               (uint32_t)fifo->buf, (uint32_t)&spi->DR, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
        LL_DMA_SetDataLength((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream, nb);
        i2s_dma_clear_tc((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
        i2s_dma_clear_ht((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
        i2s_dma_apply_irq_mode((DMA_TypeDef*)host->cfg.dma_tx.dma_handle,
                               host->cfg.dma_tx.dma_stream, host->irq_mode);
        LL_SPI_EnableDMAReq_TX(spi);
        LL_DMA_EnableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
    }
    if (rx_enable)
    {
        if (!host->cfg.dma_rx.dma_enable || !host->cfg.dma_rx.dma_handle)
            return MINI_ERR_NOTSUPP;
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
        LL_DMA_SetMode((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream, LL_DMA_MODE_CIRCULAR);
        LL_DMA_ConfigAddresses((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream,
                               (uint32_t)&spi->DR, (uint32_t)fifo->buf, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
        LL_DMA_SetDataLength((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream, nb);
        i2s_dma_clear_tc((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
        i2s_dma_clear_ht((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
        i2s_dma_apply_irq_mode((DMA_TypeDef*)host->cfg.dma_rx.dma_handle,
                               host->cfg.dma_rx.dma_stream, host->irq_mode);
        LL_SPI_EnableDMAReq_RX(spi);
        LL_DMA_EnableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
    }
    if (host->irq_mode != HAL_I2S_IRQ_NONE)
        i2s_bind_bottom_half(pdev);

    host->circ_running = 1;
    return MINI_OK;
}

/**
 * @brief 停止 I2S circular DMA 并关闭 HT/TC 中断
 * @param pdev I2S 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2s_dma_circ_stop(struct hal_i2s_dev* pdev)
{
    struct hal_i2s_bus_host* host;
    SPI_TypeDef* spi;

    if (!pdev || !pdev->ctlr)
        return MINI_ERR_INVAL;
    host = pdev->ctlr;
    if (!host->circ_running)
        return MINI_OK;
    spi = (SPI_TypeDef*)host->spi;
    if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
    {
        i2s_dma_apply_irq_mode((DMA_TypeDef*)host->cfg.dma_tx.dma_handle,
                               host->cfg.dma_tx.dma_stream, HAL_I2S_IRQ_NONE);
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);
        LL_SPI_DisableDMAReq_TX(spi);
    }
    if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
    {
        i2s_dma_apply_irq_mode((DMA_TypeDef*)host->cfg.dma_rx.dma_handle,
                               host->cfg.dma_rx.dma_stream, HAL_I2S_IRQ_NONE);
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);
        LL_SPI_DisableDMAReq_RX(spi);
    }
    host->circ_running = 0;
    return MINI_OK;
}

/**
 * @brief 向 circular 缓冲线性写入 TX 采样 (供 DMA 循环播放)
 * @param pdev I2S 设备指针
 * @param data 采样数据源
 * @param samples 采样点数 (超出 fifo 容量时截断)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_i2s_dma_circ_write(struct hal_i2s_dev* pdev, const uint16_t* data, uint32_t samples)
{
    struct fifo_spsc* fifo;
    uint32_t i, n;

    if (!pdev || !pdev->ctlr || !data || samples == 0)
        return MINI_ERR_INVAL;
    fifo = pdev->ctlr->cfg.circ_fifo;
    if (!fifo || !fifo->buf)
        return MINI_ERR_INVAL;
    /* 对齐 DAC: 线性填入底层 buf (供 circular DMA 循环播放) */
    n = samples;
    if (n > fifo->size)
        n = fifo->size;
    for (i = 0; i < n; i++)
        fifo->buf[i] = (fifo_data_type)data[i];
    return MINI_OK;
}

/**
 * @brief 从 circular 缓冲线性读出 RX 采样快照
 * @param pdev I2S 设备指针
 * @param data 输出缓冲
 * @param samples 读取采样点数 (超出 fifo 容量时截断)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_i2s_dma_circ_read(struct hal_i2s_dev* pdev, uint16_t* data, uint32_t samples)
{
    struct fifo_spsc* fifo;
    uint32_t i, n;

    if (!pdev || !pdev->ctlr || !data || samples == 0)
        return MINI_ERR_INVAL;
    fifo = pdev->ctlr->cfg.circ_fifo;
    if (!fifo || !fifo->buf)
        return MINI_ERR_INVAL;
    n = samples;
    if (n > fifo->size)
        n = fifo->size;
    for (i = 0; i < n; i++)
        data[i] = (uint16_t)fifo->buf[i];
    return MINI_OK;
}

/**
 * @brief 设置 DMA HT/TC 中断模式 (circular 运行中返回 BUSY)
 * @param pdev I2S 设备指针
 * @param irq_mode HAL_I2S_IRQ_NONE / HT / TC / HT_TC
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_BUSY
 */
int hal_i2s_set_dma_irq_mode(struct hal_i2s_dev* pdev, uint32_t irq_mode)
{
    if (!pdev || !pdev->ctlr)
        return MINI_ERR_INVAL;
    if (irq_mode > HAL_I2S_IRQ_HT_TC)
        return MINI_ERR_INVAL;
    /* circular 运行中改模式需先 stop; 此处仅存软件态, 下次 start/async 生效 */
    if (pdev->ctlr->circ_running)
        return MINI_ERR_BUSY;
    pdev->ctlr->irq_mode = irq_mode;
    return MINI_OK;
}

/**
 * @brief 读取当前 DMA 中断模式
 * @param pdev I2S 设备指针
 * @param irq_mode 输出中断模式
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_i2s_get_dma_irq_mode(struct hal_i2s_dev* pdev, uint32_t* irq_mode)
{
    if (!pdev || !pdev->ctlr || !irq_mode)
        return MINI_ERR_INVAL;
    *irq_mode = pdev->ctlr->irq_mode;
    return MINI_OK;
}

/**
 * @brief 提交 I2S 异步传输 (占位: 保存参数, DMA 启动待补)
 * @param pdev I2S 设备指针
 * @param tx 发送采样缓冲 (可为 NULL)
 * @param rx 接收采样缓冲 (可为 NULL)
 * @param samples 采样点数
 * @param cb 完成回调
 * @param userdata 用户数据指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_BUSY
 */
int hal_i2s_transfer_async(struct hal_i2s_dev* pdev, const uint16_t* tx, uint16_t* rx, size_t samples,
                          hal_i2s_callback_t cb, void* userdata)
{
    if (!pdev || !pdev->hw_open || !pdev->ctlr)
        return MINI_ERR_INVAL;
    if (samples == 0 || (!tx && !rx))
        return MINI_ERR_INVAL;
    if (pdev->async_pending)
        return MINI_ERR_BUSY;

    /* 占位: 仅保存参数; 真正 DMA NORMAL + HT/TC IT 启动与完成回调后续补 */
    pdev->async_tx = tx;
    pdev->async_rx = rx;
    pdev->async_samples = samples;
    pdev->async_cb = cb;
    pdev->async_user = userdata;
    pdev->async_pending = 1;

    /*
     * TODO(async DMA start):
     *   - 按 tx/rx 配 DMA NORMAL 地址/长度
     *   - i2s_dma_apply_irq_mode(..., host->irq_mode)  通常 TC 或 HT_TC
     *   - EnableDMAReq + EnableStream
     *   - 完成在虚拟上半部清标志 → 下半部调 async_cb / 清 async_pending
     */
    (void)cb;
    (void)userdata;
    return MINI_OK;
}

/**
 * @brief 轮询异步传输完成 (占位: 未真正启动 DMA 时返回 NOTSUPP)
 * @param pdev I2S 设备指针
 * @param timeout_ms 超时 (ms)
 * @return 无 pending 返回 MINI_OK, 否则返回 MINI_ERR_NOTSUPP 或 MINI_ERR_INVAL
 */
int hal_i2s_transfer_poll(struct hal_i2s_dev* pdev, uint32_t timeout_ms)
{
    if (!pdev || !pdev->hw_open)
        return MINI_ERR_INVAL;
    if (!pdev->async_pending)
        return MINI_OK;

    /*
     * 占位: 本应轮询 async 完成或等下半部清 pending。
     * 当前未真正启动 DMA, 直接返回 NOTSUPP 避免假完成。
     */
    COMPAT_IGNORE_RESULT(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/* =========================================================================================================================================================== */
/* ISR 虚拟中断回调 (上半部) / DMA 下半部 — 对齐 ADC                                                                                                           */
/* =========================================================================================================================================================== */

/**
 * @brief I2S 虚拟中断上半部回调 (ISR 内执行)
 * @param arg I2S 设备指针 (hal_i2s_dev*)
 * @param irq_num 虚拟中断号
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部; MINI_IRQ_ENTRY_NOBOTTOM 不需要
 */
int hal_virtual_i2s_irq_callback(void* arg, uint16_t irq_num)
{
    struct hal_i2s_dev* pdev = (struct hal_i2s_dev*)arg;

    COMPAT_IGNORE_RESULT(irq_num);

    if (!pdev || !pdev->ctlr)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    if (pdev->ctlr->irq_mode == HAL_I2S_IRQ_NONE)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    /** 需要下半部: bus open 经 g_i2s_bottom_half_work 注册; HT/TC 清零在 IRQHandler */
    return MINI_IRQ_ENTRY_BOTTOM;
}

/**
 * @brief I2S DMA 下半部处理 (任务上下文, HT/TC/async 占位)
 * @param arg I2S 设备指针 (hal_i2s_dev*)
 */
void hal_i2s_dma_bottom_half_handler(void* arg)
{
    struct hal_i2s_dev* pdev = (struct hal_i2s_dev*)arg;

    if (!pdev || !pdev->ctlr)
        return;

    /*
     * 下半部占位 (主循环/任务上下文, 对齐 ADC dma bottom half):
     *   - HT: 填/取环形缓冲半区
     *   - TC: 另一半; async 路径调 async_cb 并清 async_pending
     */
    COMPAT_IGNORE_RESULT(pdev);
}
