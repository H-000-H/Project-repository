/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPI HAL — STM32F4 实现 (Master only)
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 LL 库。
 * - hal_spi_bus_host 嵌入 bus 层, HAL 无 s_spi_hosts[] 池
 * - hal_spi_sync: CS 变更检测用 cs_port + cs_pin 直接比较 (多设备共线不打架)
 * - slave / async 返回 MINI_ERR_NOTSUPP
 */
#include "hal_spi.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_spi.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_dma.h"
#include "interrupt.h"

#include "dt_config_gen.h"

/** SPI 下半部工作项 (fn/arg 由 VFS 层绑定), 供 interrupt_virtual_register 注册 */
struct bottom_half_work g_spi_bottom_half_work;

/** 平台参数：兼容 DTC_GEN_SPI_* 与 DTC_GEN_STM32_SPI_*，无 DTS 时回退 */
#if defined(DTC_GEN_SPI_HOST_MAX) && !defined(DTC_GEN_STM32_SPI_HOST_MAX)
#define DTC_GEN_STM32_SPI_HOST_MAX  DTC_GEN_SPI_HOST_MAX
#endif
#if defined(DTC_GEN_SPI_MAX_XFER) && !defined(DTC_GEN_STM32_SPI_MAX_XFER)
#define DTC_GEN_STM32_SPI_MAX_XFER  DTC_GEN_SPI_MAX_XFER
#endif
#ifndef DTC_GEN_STM32_SPI_HOST_MAX
#define DTC_GEN_STM32_SPI_HOST_MAX  3
#endif
#ifndef DTC_GEN_STM32_SPI_MAX_XFER
#define DTC_GEN_STM32_SPI_MAX_XFER  512U
#endif

/** 覆盖 hal_spi.h 中的默认值，避免宏重定义警告/错误 */
#undef HAL_SPI_HOST_MAX
#define HAL_SPI_HOST_MAX   DTC_GEN_STM32_SPI_HOST_MAX
#undef HAL_SPI_MAX_XFER
#define HAL_SPI_MAX_XFER   DTC_GEN_STM32_SPI_MAX_XFER

/* per-host dummy buffer: DMA 路径下 tx/rx 为 NULL 时填充占位, 防 cache line 踩踏。
 * - s_dummy_tx 填 0xFF: 用户只收时, DMA 仍需往 SPI->DR 写驱动 SCLK
 * - s_dummy_rx 丢弃区:  用户只发时, DMA 仍需从 SPI->DR 读避免 OVR
 * 32 字节对齐适配 DMA cache line; per-host 索引防多 host 并发踩踏。 */
static uint8_t s_dummy_tx[HAL_SPI_HOST_MAX][HAL_SPI_MAX_XFER] MINI_ALIGNED(32);
static uint8_t s_dummy_rx[HAL_SPI_HOST_MAX][HAL_SPI_MAX_XFER] MINI_ALIGNED(32);

/* 纯 LL 库调用, 非抽象层 */
/**
 * @brief 配置 SPI 复用引脚: 时钟使能 + AF 模式 + 推挽高速 (LL 库直投)
 * @param pin 引脚配置 (含 port/pin/clk_bus/af)
 */
MINI_STATIC_INLINE void hal_spi_config_af_pin(const struct hal_spi_pin_cfg* pin)
{
    GPIO_TypeDef* port = (GPIO_TypeDef*)pin->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin        = pin->pin;
    /* DTS 未写 mode(=0) 时默认为 AF, 避免配成 INPUT */
    GPIO_InitStruct.Mode       = pin->mode ? pin->mode : LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Pull       = pin->pull;
    GPIO_InitStruct.Alternate  = pin->af;
    GPIO_InitStruct.Speed      = pin->speed ? pin->speed : LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = pin->output_type;

    LL_AHB1_GRP1_EnableClock(pin->clk_bus);
    LL_GPIO_Init(port, &GPIO_InitStruct);
}

/**
 * @brief 复位 SPI 复用引脚为模拟模式 + 无上下拉 (等效去初始化)
 * @param pin 引脚配置
 */
static void hal_spi_reset_af_pin(const struct hal_spi_pin_cfg* pin)
{
    GPIO_TypeDef* port = (GPIO_TypeDef*)pin->port;
    LL_GPIO_SetPinMode(port, pin->pin, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinPull(port, pin->pin, LL_GPIO_PULL_NO);
}

/**
 * @brief 由目标时钟频率选最近档位的 LL 波特率预分频值
 * @param clock_hz 目标时钟频率 (Hz), <=0 时返回最大分频
 * @return LL_SPI_BAUDRATEPRESCALER_DIV* 宏值
 */
static uint32_t stm32_spi_prescaler(int clock_hz)
{
    if (clock_hz <= 0)
        return LL_SPI_BAUDRATEPRESCALER_DIV256;
    if (clock_hz >= 21000000) return LL_SPI_BAUDRATEPRESCALER_DIV2;
    if (clock_hz >= 10500000) return LL_SPI_BAUDRATEPRESCALER_DIV4;
    if (clock_hz >= 5250000)  return LL_SPI_BAUDRATEPRESCALER_DIV8;
    if (clock_hz >= 2625000)  return LL_SPI_BAUDRATEPRESCALER_DIV16;
    if (clock_hz >= 1312500)  return LL_SPI_BAUDRATEPRESCALER_DIV32;
    if (clock_hz >= 656250)   return LL_SPI_BAUDRATEPRESCALER_DIV64;
    if (clock_hz >= 328125)   return LL_SPI_BAUDRATEPRESCALER_DIV128;
    return LL_SPI_BAUDRATEPRESCALER_DIV256;
}

/**
 * @brief 轮询等待 SPI BSY 标志清零
 * @param spi SPI 外设寄存器
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int hal_spi_wait_idle(SPI_TypeDef* spi, uint32_t timeout_ms)
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
 * @brief 按 SPI 基址使能 APB1/APB2 外设时钟
 * @param spi_base SPI 控制器基址 (SPI1→APB2, SPI2/SPI3→APB1)
 * @param clk_periph LL 时钟外设掩码
 */
static void hal_spi_enable_periph_clock(uintptr_t spi_base, uint32_t clk_periph)
{
    if (spi_base == SPI1_BASE)
        LL_APB2_GRP1_EnableClock(clk_periph);
    else
        LL_APB1_GRP1_EnableClock(clk_periph);
}

/**
 * @brief 软 NSS: 初始化 CS GPIO 为推挽输出并默认拉高 (inactive)
 * @param cfg 设备配置 (含 cs_port/cs_pin/cs_clk_periph), 无效时跳过
 */
static void hal_spi_cs_gpio_init(const struct hal_spi_device_config* cfg)
{
    LL_GPIO_InitTypeDef init = {0};

    if (!cfg || !cfg->cs_port || cfg->cs_pin < 0)
        return;

    if (cfg->cs_clk_periph)
        LL_AHB1_GRP1_EnableClock(cfg->cs_clk_periph);

    init.Pin        = cfg->cs_pin;
    init.Mode       = LL_GPIO_MODE_OUTPUT;
    init.Speed      = LL_GPIO_SPEED_FREQ_HIGH;
    init.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    init.Pull       = LL_GPIO_PULL_NO;
    LL_GPIO_Init((GPIO_TypeDef*)cfg->cs_port, &init);
    LL_GPIO_SetOutputPin((GPIO_TypeDef*)cfg->cs_port, cfg->cs_pin);
}

/**
 * @brief 拉低 CS (软件片选有效)
 * @param cfg 设备配置指针 (含 cs_port/cs_pin), 无效时跳过
 */
static void hal_spi_cs_assert(const struct hal_spi_device_config* cfg)
{
    if (!cfg || !cfg->cs_port || cfg->cs_pin < 0)
        return;
    LL_GPIO_ResetOutputPin((GPIO_TypeDef*)cfg->cs_port, cfg->cs_pin);
}

/**
 * @brief 拉高 CS (软件片选释放)
 * @param cfg 设备配置指针 (含 cs_port/cs_pin), 无效时跳过
 */
static void hal_spi_cs_deassert(const struct hal_spi_device_config* cfg)
{
    if (!cfg || !cfg->cs_port || cfg->cs_pin < 0)
        return;
    LL_GPIO_SetOutputPin((GPIO_TypeDef*)cfg->cs_port, cfg->cs_pin);
}

/*============================================================================*/
/*                              Host 管理 API                                 */
/*============================================================================*/
/** forward: DMA 静态参数配置 (定义在 DMA 区, host_init 提前调用) */
static void hal_spi_dma_init(const struct hal_spi_dma_config* cfg, uint32_t direction);

/**
 * @brief 初始化 SPI 总线主机 (时钟/GPIO/DMA 静态配置)
 * @param host 总线主机对象指针
 * @param hw_idx dummy 缓冲 / HW slot 索引
 * @param cfg 总线配置 (硬件直投)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_NODEV
 */
int hal_spi_bus_host_init(struct hal_spi_bus_host* host, int hw_idx, const struct hal_spi_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= HAL_SPI_HOST_MAX)
        return MINI_ERR_INVAL;
    if (host->bus_ready)
        return MINI_OK;
    if (!cfg->spi)
        return MINI_ERR_NODEV;

    MINI_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;

 if (host->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER && host->cfg.bus_role != HAL_SPI_BUS_ROLE_SLAVE)
        host->cfg.bus_role = HAL_SPI_BUS_ROLE_MASTER;
    /* 上限与 dummy/DMA 缓冲一致, 避免 sync 过检后 poll 再拒 */
    if (host->cfg.max_transfer_sz <= 0)
        host->cfg.max_transfer_sz = (size_t)HAL_SPI_MAX_XFER;
    else if (host->cfg.max_transfer_sz > (size_t)HAL_SPI_MAX_XFER)
        host->cfg.max_transfer_sz = (size_t)HAL_SPI_MAX_XFER;

    hal_spi_enable_periph_clock(cfg->spi, cfg->spi_clk_periph);

    hal_spi_config_af_pin(&cfg->mosi);
    hal_spi_config_af_pin(&cfg->miso);
    hal_spi_config_af_pin(&cfg->sclk);

    /** DMA 静态参数一次性配置: TX=mem→periph, RX=periph→mem; dma_enable=0 时跳过 */
    if (cfg->dma_tx.dma_enable)
        hal_spi_dma_init(&cfg->dma_tx, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    if (cfg->dma_rx.dma_enable)
        hal_spi_dma_init(&cfg->dma_rx, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

    /*<缓存 fast path 字段 */
    host->spi      = cfg->spi;
    host->hw_idx   = hw_idx;
    host->bus_ready = true;
    return MINI_OK;
}

/**
 * @brief 释放 SPI 总线主机 (关 SPI、复位 AF 引脚)
 * @param host 总线主机对象指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_spi_bus_host_deinit(struct hal_spi_bus_host* host)
{
    if (!host)
        return MINI_ERR_INVAL;
    if (!host->bus_ready)
        return MINI_OK;

    LL_SPI_Disable((SPI_TypeDef*)host->spi);
    /* 引脚复位为 analog (同 GPIO deinit 模式) */
    hal_spi_reset_af_pin(&host->cfg.mosi);
    hal_spi_reset_af_pin(&host->cfg.miso);
    hal_spi_reset_af_pin(&host->cfg.sclk);

    host->bus_ready = false;
    return MINI_OK;
}

/*============================================================================*/
/*                              Device 管理 API                               */
/*============================================================================*/
/**
 * @brief 初始化 SPI 设备对象 (绑定 host 与设备配置)
 * @param dev 设备对象指针
 * @param host 总线主机对象指针
 * @param dev_cfg 设备配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_spi_dev_init(struct hal_spi_dev* dev,struct hal_spi_bus_host* host,const struct hal_spi_device_config* dev_cfg)
{
    if (!dev || !host || !dev_cfg)
        return MINI_ERR_INVAL;

    MINI_MEM_SET(dev, 0, sizeof(*dev));
    dev->ctlr     = host;
    dev->cfg      = *dev_cfg;
    return MINI_OK;
}

/**
 * @brief 将 device 配置应用到 SPI 硬件: 关 SPI → 填 LL_SPI_InitTypeDef → 重启 SPI
 * @param host     Host 对象指针
 * @param dev_cfg  设备配置 (mode/clock_speed_hz)
 * @return 成功返回 MINI_OK, 参数非法或 LL_SPI_Init 失败返回 MINI_ERR_IO
 */
static int stm32_spi_apply_dev_cfg(struct hal_spi_bus_host* host,const struct hal_spi_device_config* dev_cfg)
{
    SPI_TypeDef*               spi;
    LL_SPI_InitTypeDef         init = {0};

    if (!host || !host->spi || !dev_cfg)
        return MINI_ERR_IO;

    spi = (SPI_TypeDef*)host->spi;

    LL_SPI_Disable(spi);
    init.TransferDirection = dev_cfg->transfer_direction; /* 0 == LL_SPI_FULL_DUPLEX */
    init.Mode              = (host->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER)? LL_SPI_MODE_MASTER : LL_SPI_MODE_SLAVE;
    init.DataWidth         = dev_cfg->data_width;         /* 0 == LL_SPI_DATAWIDTH_8BIT */
    init.ClockPolarity     = (dev_cfg->mode & 2) ? LL_SPI_POLARITY_HIGH : LL_SPI_POLARITY_LOW;
    init.ClockPhase        = (dev_cfg->mode & 1) ? LL_SPI_PHASE_2EDGE : LL_SPI_PHASE_1EDGE;
    /* GPIO CS 路径必须软 NSS; DTS 未写 nss(=0) 且有 cs_port 时强制 SOFT */
    if (dev_cfg->cs_port && dev_cfg->nss == 0)
        init.NSS = LL_SPI_NSS_SOFT;
    else
        init.NSS = dev_cfg->nss;
    init.BaudRate          = stm32_spi_prescaler(dev_cfg->clock_speed_hz);
    init.BitOrder          = dev_cfg->bit_order;
    init.CRCCalculation    = dev_cfg->crc_calculation;
    init.CRCPoly           = dev_cfg->crc_poly;

    if (LL_SPI_Init(spi, &init) != SUCCESS)
        return MINI_ERR_IO;

    LL_SPI_SetStandard(spi, dev_cfg->standard);
    LL_SPI_Enable(spi);

    if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
        LL_SPI_EnableDMAReq_TX(spi);
    if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
        LL_SPI_EnableDMAReq_RX(spi);

    return MINI_OK;
}

/**
 * @brief 打开 SPI 设备 (应用 dev 配置、初始化 CS GPIO)
 * @param dev 设备对象指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_IO
 */
int hal_spi_dev_hw_open(struct hal_spi_dev* dev)
{
    struct hal_spi_bus_host* host;
    int                      ret;

    if (!dev || !dev->ctlr)
        return MINI_ERR_INVAL;

    if (dev->hw_open)
        return MINI_OK;

    host = dev->ctlr;
    if (!host->bus_ready)
        return MINI_ERR_INVAL;

    ret = stm32_spi_apply_dev_cfg(host, &dev->cfg);
    if (ret != MINI_OK)
        return ret;

    hal_spi_cs_gpio_init(&dev->cfg);

    dev->hw_open = 1;
    host->ref_count++;
    host->active_cfg = dev->cfg;
    return MINI_OK;
}

/**
 * @brief 关闭 SPI 设备 (递减 host 引用计数)
 * @param dev 设备对象指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_spi_dev_hw_close(struct hal_spi_dev* dev)
{
    struct hal_spi_bus_host* host;

    if (!dev || !dev->ctlr)
        return MINI_ERR_INVAL;

    if (!dev->hw_open)
        return MINI_OK;

    host = dev->ctlr;
    if (host->ref_count > 0)
        host->ref_count--;

    dev->hw_open = 0;
    return MINI_OK;
}

/*============================================================================*/
/*                              同步传输 (Master)                             */
/*============================================================================*/
/**
 * @brief SPI 轮询传输: 逐字节 TXE/RXNE 标志轮询, 超时检测基于 HAL_GetTick
 * @param host        Host 对象指针
 * @param tx          发送缓冲区 (可为 NULL, 内部填 0xFF)
 * @param rx          接收缓冲区 (可为 NULL, 仅丢弃)
 * @param len         传输字节数
 * @param timeout_ms  超时 (ms)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL, 超时返回 MINI_ERR_TIMEOUT, 外设异常返回 MINI_ERR_IO
 */
static int spi_wait_flag(SPI_TypeDef* spi, uint32_t (*is_set)(const SPI_TypeDef*), uint32_t start, uint32_t timeout_ms, const struct hal_spi_device_config* dev_cfg)
{
    while (!is_set(spi))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            hal_spi_cs_deassert(dev_cfg);
            return MINI_ERR_TIMEOUT;
        }
    }
    return MINI_OK;
}

/**
 * @brief SPI 轮询同步传输 (TXE/RXNE + 超时)
 * @param host Host 对象指针
 * @param dev_cfg 设备配置指针
 * @param tx 发送缓冲 (可为 NULL, 内部填 0xFF)
 * @param rx 接收缓冲 (可为 NULL, 仅丢弃)
 * @param len 传输字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL, 超时返回 MINI_ERR_TIMEOUT
 */
static int stm32_spi_transfer_poll(struct hal_spi_bus_host* host, const struct hal_spi_device_config* dev_cfg, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    SPI_TypeDef* spi;
    uint32_t     start;
    uint32_t     dir;
    size_t       i;
    int          ret;
    int          half_tx;
    int          half_rx;

    if (!host || !dev_cfg || len == 0 || len > HAL_SPI_MAX_XFER || !host->spi)
        return MINI_ERR_INVAL;

    spi     = (SPI_TypeDef*)host->spi;
    dir     = dev_cfg->transfer_direction;
    half_tx = (dir == LL_SPI_HALF_DUPLEX_TX);
    half_rx = (dir == LL_SPI_HALF_DUPLEX_RX);
    if ((half_tx && !tx) || (half_rx && !rx))
        return MINI_ERR_INVAL;

    const uint32_t want_dir = half_tx ? LL_SPI_HALF_DUPLEX_TX : half_rx ? LL_SPI_HALF_DUPLEX_RX : LL_SPI_FULL_DUPLEX;
    if (LL_SPI_GetTransferDirection(spi) != want_dir)
        LL_SPI_SetTransferDirection(spi, want_dir);
    start = HAL_GetTick();
    hal_spi_cs_assert(dev_cfg);

    for (i = 0; i < len; i++)
    {
        if (!half_rx)
        {
            ret = spi_wait_flag(spi, LL_SPI_IsActiveFlag_TXE, start, timeout_ms, dev_cfg);
            if (ret != MINI_OK)
                return ret;
            LL_SPI_TransmitData8(spi, tx ? tx[i] : 0xFFU);
        }
        if (!half_tx)
        {
            ret = spi_wait_flag(spi, LL_SPI_IsActiveFlag_RXNE, start, timeout_ms, dev_cfg);
            if (ret != MINI_OK)
                return ret;
            if (rx)
                rx[i] = LL_SPI_ReceiveData8(spi);
            else
                MINI_IGNORE_RESULT(LL_SPI_ReceiveData8(spi));
        }
    }

    ret = hal_spi_wait_idle(spi, timeout_ms);
    hal_spi_cs_deassert(dev_cfg);
    return ret;
}

int hal_spi_transfer_dma_stm32(struct hal_spi_bus_host* host, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms);

/**
 * @brief SPI 同步传输 (poll / DMA / AUTO, 仅 Master)
 * @param dev 设备对象指针
 * @param tx 发送缓冲 (可为 NULL, 内部填 0xFF)
 * @param rx 接收缓冲 (可为 NULL, 仅丢弃)
 * @param len 传输字节数
 * @param timeout_ms 超时 (ms)
 * @param xfer_mode HAL_SPI_XFER_AUTO / POLL / DMA
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_spi_sync(struct hal_spi_dev* dev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms, uint32_t xfer_mode)
{
    if (!dev || !dev->ctlr || !dev->hw_open || len == 0)
        return MINI_ERR_INVAL;

    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER)
        return MINI_ERR_INVAL;

    if (len > (size_t)dev->ctlr->cfg.max_transfer_sz)
        return MINI_ERR_INVAL;

    if (xfer_mode > HAL_SPI_XFER_DMA)
        return MINI_ERR_INVAL;

    /* 多设备共线时 cfg 变化则重配 */
    if (__builtin_memcmp(&dev->ctlr->active_cfg, &dev->cfg, sizeof(dev->cfg)) != 0)
    {
        int ret = stm32_spi_apply_dev_cfg(dev->ctlr, &dev->cfg);
        if (ret != MINI_OK)
            return ret;
        hal_spi_cs_gpio_init(&dev->cfg);
        dev->ctlr->active_cfg = dev->cfg;
    }

    if (xfer_mode == HAL_SPI_XFER_POLL)
        return stm32_spi_transfer_poll(dev->ctlr, &dev->cfg, tx, rx, len, timeout_ms);

    if (xfer_mode == HAL_SPI_XFER_DMA)
    {
        int ret = hal_spi_transfer_dma_stm32(dev->ctlr, tx, rx, len, timeout_ms);
        return (ret == MINI_ERR_NOTSUPP) ? MINI_ERR_NOTSUPP : ret;
    }

    /* AUTO: DMA 通道齐全则优先 DMA, 否则 poll */
    {
        uint32_t dir = dev->cfg.transfer_direction;
        int dma_ok = (dir == LL_SPI_HALF_DUPLEX_RX || dev->ctlr->cfg.dma_tx.dma_enable) && (dir == LL_SPI_HALF_DUPLEX_TX || dev->ctlr->cfg.dma_rx.dma_enable);
        if (dma_ok)
        {
            int ret = hal_spi_transfer_dma_stm32(dev->ctlr, tx, rx, len, timeout_ms);
            if (ret != MINI_ERR_NOTSUPP)
                return ret;
        }
    }

    return stm32_spi_transfer_poll(dev->ctlr, &dev->cfg, tx, rx, len, timeout_ms);
}

/* SPI 全双工 DMA: TX (mem→periph) + RX (periph→mem) 双通道, poll 等 TX TC + BSY。
 * 配置来自 host->cfg.dma_tx / dma_rx (DTS 硬件直投), HAL 零翻译灌入 LL_DMA。 */

/**
 * @brief 清除 DMA stream 对应的 TC 标志 (STM32F4 LL 库按 stream 编号分布)
 * @param dma DMA 控制器基址
 * @param stream DMA 流编号 (0..7), 越界时跳过
 */
static void hal_spi_dma_clear_tc_optimized(DMA_TypeDef* dma, uint32_t stream)
{
    static void (*const clear_tc_funcs[])(DMA_TypeDef*) = {
        LL_DMA_ClearFlag_TC0, LL_DMA_ClearFlag_TC1, LL_DMA_ClearFlag_TC2, LL_DMA_ClearFlag_TC3,
        LL_DMA_ClearFlag_TC4, LL_DMA_ClearFlag_TC5, LL_DMA_ClearFlag_TC6, LL_DMA_ClearFlag_TC7
    };

    /*边界检查，防止 Stream 索引越界（假设 LL_DMA_STREAM_0 为 0，依次递增）*/
    if (stream <= LL_DMA_STREAM_7) 
    {
        clear_tc_funcs[stream](dma);
    }
}
/**
 * @brief 检查 DMA 传输是否完成
 * @param dma DMA 控制器
 * @param stream DMA 流
 * @return true 完成, false 未完成
 */
static bool hal_spi_dma_is_active_tc(DMA_TypeDef* dma, uint32_t stream)
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

#define SPI_OR_DEF(v, d) ((v) ? (v) : (d))

/**
 * @brief 按 DMA 基址使能 AHB1 DMA1/DMA2 时钟
 * @param dma_base DMA 控制器基址 (DMA1_BASE 或 DMA2_BASE), 非法时跳过
 */
static void hal_spi_enable_dma_clock(uintptr_t dma_base)
{
    uint32_t periph = (dma_base == DMA1_BASE) ? LL_AHB1_GRP1_PERIPH_DMA1 : (dma_base == DMA2_BASE) ? LL_AHB1_GRP1_PERIPH_DMA2 : 0U;
    if (periph)
        LL_AHB1_GRP1_EnableClock(periph);
}

/**
 * @brief DMA 静态参数一次性配置 (host_init 时调用, 热路径不再重复)
 * @param cfg DMA 配置 (来自 DTS), 无效或 dma_handle 为空时跳过
 * @param direction LL_DMA 传输方向 (mem↔periph)
 * @note  channel/direction/priority/mode/inc/size 来自 DTS 且永不变;
 *        仅 buffer 地址与长度每次传输不同, 留在 transfer_dma 热路径。
 */
static void hal_spi_dma_init(const struct hal_spi_dma_config* cfg, uint32_t direction)
{
    LL_DMA_InitTypeDef init = {0};

    if (!cfg || !cfg->dma_handle)
        return;

    hal_spi_enable_dma_clock(cfg->dma_handle);

    /** STM32F4 要求配置前 stream disable */
    LL_DMA_DisableStream((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream);

    init.PeriphOrM2MSrcAddress   = 0;          /* 动态: transfer_dma 热路径设 */
    init.MemoryOrM2MDstAddress   = 0;          /* 动态: transfer_dma 热路径设 */
    init.Direction               = direction;
    init.Mode                    = cfg->dma_mode; /* 0 == NORMAL */
    init.PeriphOrM2MSrcIncMode   = cfg->dma_periph_inc;
    init.MemoryOrM2MDstIncMode   = SPI_OR_DEF(cfg->dma_mem_inc, LL_DMA_MEMORY_INCREMENT);
    init.PeriphOrM2MSrcDataSize  = SPI_OR_DEF(cfg->dma_periph_data_size, LL_DMA_PDATAALIGN_BYTE);
    init.MemoryOrM2MDstDataSize  = SPI_OR_DEF(cfg->dma_memory_size, LL_DMA_MDATAALIGN_BYTE);
    init.NbData                  = 0;          /* 动态: transfer_dma 热路径设 */
    init.Channel                 = cfg->dma_channel;
    init.Priority                = cfg->dma_priority;
    init.FIFOMode                = cfg->dma_fifo_mode;
    init.FIFOThreshold           = cfg->dma_fifo_threshold;
    init.MemBurst                = cfg->dma_mem_burst;
    init.PeriphBurst             = cfg->dma_periph_burst;
    LL_DMA_Init((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream, &init);
}

/**
 * @brief SPI DMA 同步传输 (TX/RX 双通道, 轮询等 TC + BSY)
 * @param host 总线主机对象指针
 * @param tx 发送缓冲 (可为 NULL, 用 per-host dummy)
 * @param rx 接收缓冲 (可为 NULL, 用 per-host dummy)
 * @param len 传输字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL / MINI_ERR_IO / MINI_ERR_NOTSUPP / MINI_ERR_TIMEOUT
 */
int hal_spi_transfer_dma_stm32(struct hal_spi_bus_host* host, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    SPI_TypeDef*   spi;
    DMA_TypeDef*   dma_tx_ctrl;
    DMA_TypeDef*   dma_rx_ctrl;
    uint32_t       tx_stream;
    uint32_t       rx_stream;
    uint32_t       start;
    uint32_t       dir;
    int            use_tx;
    int            use_rx;
    const uint8_t* tx_buf = tx;
    uint8_t*       rx_buf = rx;

    if (!host || len == 0)
        return MINI_ERR_INVAL;
    if (len > HAL_SPI_MAX_XFER)
        return MINI_ERR_INVAL;

    spi = (SPI_TypeDef*)host->spi;
    if (!spi)
        return MINI_ERR_IO;

    dir    = host->active_cfg.transfer_direction;
    use_tx = (dir != LL_SPI_HALF_DUPLEX_RX);
    use_rx = (dir != LL_SPI_HALF_DUPLEX_TX);

    if ((use_tx && !host->cfg.dma_tx.dma_enable) || (use_rx && !host->cfg.dma_rx.dma_enable))
        return MINI_ERR_NOTSUPP;
    if ((dir == LL_SPI_HALF_DUPLEX_TX && !tx) || (dir == LL_SPI_HALF_DUPLEX_RX && !rx))
        return MINI_ERR_INVAL;

    dma_tx_ctrl = (DMA_TypeDef*)host->cfg.dma_tx.dma_handle;
    dma_rx_ctrl = (DMA_TypeDef*)host->cfg.dma_rx.dma_handle;
    tx_stream   = host->cfg.dma_tx.dma_stream;
    rx_stream   = host->cfg.dma_rx.dma_stream;
    if ((use_tx && !dma_tx_ctrl) || (use_rx && !dma_rx_ctrl))
        return MINI_ERR_IO;

    const uint32_t want_dir = (use_tx && !use_rx) ? LL_SPI_HALF_DUPLEX_TX : (use_rx && !use_tx) ? LL_SPI_HALF_DUPLEX_RX : LL_SPI_FULL_DUPLEX;
    if (LL_SPI_GetTransferDirection(spi) != want_dir)
        LL_SPI_SetTransferDirection(spi, want_dir);

    if (use_tx && !tx_buf)
    {
        MINI_MEM_SET(s_dummy_tx[host->hw_idx], 0xFF, len);
        tx_buf = s_dummy_tx[host->hw_idx];
    }
    if (use_rx && !rx_buf)
        rx_buf = s_dummy_rx[host->hw_idx];

    hal_spi_cs_assert(&host->active_cfg);

    if (use_tx)
    {
        LL_DMA_DisableStream(dma_tx_ctrl, tx_stream);
        LL_DMA_ConfigAddresses(dma_tx_ctrl, tx_stream, (uint32_t)tx_buf, (uint32_t)&spi->DR, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
        LL_DMA_SetDataLength(dma_tx_ctrl, tx_stream, len);
        hal_spi_dma_clear_tc_optimized(dma_tx_ctrl, tx_stream);
    }
    if (use_rx)
    {
        LL_DMA_DisableStream(dma_rx_ctrl, rx_stream);
        LL_DMA_ConfigAddresses(dma_rx_ctrl, rx_stream, (uint32_t)&spi->DR, (uint32_t)rx_buf, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
        LL_DMA_SetDataLength(dma_rx_ctrl, rx_stream, len);
        hal_spi_dma_clear_tc_optimized(dma_rx_ctrl, rx_stream);
    }
    if (use_rx)
        LL_DMA_EnableStream(dma_rx_ctrl, rx_stream);
    if (use_tx)
        LL_DMA_EnableStream(dma_tx_ctrl, tx_stream);

    start = HAL_GetTick();
    while ((use_tx && !hal_spi_dma_is_active_tc(dma_tx_ctrl, tx_stream)) ||
           (use_rx && !hal_spi_dma_is_active_tc(dma_rx_ctrl, rx_stream)))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            goto timeout;
    }

    if (hal_spi_wait_idle(spi, timeout_ms) != MINI_OK)
        goto timeout;

    if (use_tx)
    {
        hal_spi_dma_clear_tc_optimized(dma_tx_ctrl, tx_stream);
        LL_DMA_DisableStream(dma_tx_ctrl, tx_stream);
    }
    if (use_rx)
    {
        hal_spi_dma_clear_tc_optimized(dma_rx_ctrl, rx_stream);
        LL_DMA_DisableStream(dma_rx_ctrl, rx_stream);
    }
    hal_spi_cs_deassert(&host->active_cfg);
    return MINI_OK;

timeout:
    if (use_tx)
    {
        LL_DMA_DisableStream(dma_tx_ctrl, tx_stream);
    }
    if (use_rx)
    {
        LL_DMA_DisableStream(dma_rx_ctrl, rx_stream);
    }
    hal_spi_cs_deassert(&host->active_cfg);
    return MINI_ERR_TIMEOUT;
}

/**
 * @brief 中止 SPI DMA 传输 (关 DMA 请求与 stream)
 * @param host 总线主机对象指针
 */
void hal_spi_abort_stm32(struct hal_spi_bus_host* host)
{
    if (!host || !host->spi)
        return;

    if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_tx.dma_handle,host->cfg.dma_tx.dma_stream);
    if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
        LL_DMA_DisableStream((DMA_TypeDef*)host->cfg.dma_rx.dma_handle,host->cfg.dma_rx.dma_stream);
}

/* =========================================================================================================================================================== */
/* ISR 虚拟中断回调                                                                                                                                              */
/* =========================================================================================================================================================== */

/**
 * @brief SPI 虚拟中断上半部回调 (ISR 内执行)
 * @param arg SPI 设备指针 (hal_spi_dev*)
 * @param irq_num 虚拟中断号
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部; MINI_IRQ_ENTRY_NOBOTTOM 不需要
 */
int hal_virtual_spi_irq_callback(void* arg, uint16_t irq_num)
{
    MINI_IGNORE_RESULT(irq_num);
    struct hal_spi_dev* dev = (struct hal_spi_dev*)arg;

    if (!dev || !dev->ctlr)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    struct hal_spi_bus_host* host = dev->ctlr;

    if (!host->cfg.it_enable)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    /** 清除 TX DMA TC 标志 */
    if (host->cfg.dma_tx.dma_enable && host->cfg.dma_tx.dma_handle)
        hal_spi_dma_clear_tc_optimized((DMA_TypeDef*)host->cfg.dma_tx.dma_handle, host->cfg.dma_tx.dma_stream);

    /** 清除 RX DMA TC 标志 */
    if (host->cfg.dma_rx.dma_enable && host->cfg.dma_rx.dma_handle)
        hal_spi_dma_clear_tc_optimized((DMA_TypeDef*)host->cfg.dma_rx.dma_handle, host->cfg.dma_rx.dma_stream);

    return MINI_IRQ_ENTRY_BOTTOM;
}

/*============================================================================*/
/*                              异步传输 (STM32 不支持, 返回 NOTSUPP)          */
/*============================================================================*/
/**
 * @brief SPI 异步传输 (STM32 不支持)
 * @param dev 设备对象指针
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param len 传输字节数
 * @param cb 完成回调
 * @param userdata 用户数据指针
 * @return 固定返回 MINI_ERR_NOTSUPP
 */
int hal_spi_transfer_async(struct hal_spi_dev* dev,const uint8_t* tx, uint8_t* rx,size_t len, hal_spi_callback_t cb,void* userdata)
{
    MINI_IGNORE_RESULT(dev);
    MINI_IGNORE_RESULT(tx);
    MINI_IGNORE_RESULT(rx);
    MINI_IGNORE_RESULT(len);
    MINI_IGNORE_RESULT(cb);
    MINI_IGNORE_RESULT(userdata);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief SPI 异步传输轮询 (STM32 不支持)
 * @param dev 设备对象指针
 * @param timeout_ms 超时 (ms)
 * @return 固定返回 MINI_ERR_NOTSUPP
 */
int hal_spi_transfer_poll(struct hal_spi_dev* dev, uint32_t timeout_ms)
{
    MINI_IGNORE_RESULT(dev);
    MINI_IGNORE_RESULT(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 获取 SPI 异步传输结果 (STM32 不支持)
 * @param dev 设备对象指针
 * @param rx_data 接收缓冲
 * @param rx_cap 接收缓冲容量
 * @param trans_len 输出实际传输长度
 * @param timeout_ms 超时 (ms)
 * @return 固定返回 MINI_ERR_NOTSUPP
 */
int hal_spi_get_trans_result(struct hal_spi_dev* dev, uint8_t* rx_data, size_t rx_cap, size_t* trans_len, uint32_t timeout_ms)
{
    MINI_IGNORE_RESULT(dev);
    MINI_IGNORE_RESULT(rx_data);
    MINI_IGNORE_RESULT(rx_cap);
    MINI_IGNORE_RESULT(trans_len);
    MINI_IGNORE_RESULT(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/*============================================================================*/
/*                              Slave 传输 (STM32 不支持, 返回 NOTSUPP)        */
/*============================================================================*/
/**
 * @brief SPI 从机同步传输 (STM32 不支持)
 * @param dev 设备对象指针
 * @param tx 发送缓冲区 (可为 NULL)
 * @param rx 接收缓冲区 (可为 NULL)
 * @param len 传输字节数
 * @param timeout_ms 超时 (ms)
 * @return 固定返回 MINI_ERR_NOTSUPP
 */
int hal_spi_slave_sync(struct hal_spi_dev* dev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    MINI_IGNORE_RESULT(dev);
    MINI_IGNORE_RESULT(tx);
    MINI_IGNORE_RESULT(rx);
    MINI_IGNORE_RESULT(len);
    MINI_IGNORE_RESULT(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief SPI 从机队列发送 (STM32 不支持)
 * @param dev 设备对象指针
 * @param data 发送缓冲
 * @param len 传输字节数
 * @param timeout_ms 超时 (ms)
 * @return 固定返回 MINI_ERR_NOTSUPP
 */
int hal_spi_slave_queue_tx(struct hal_spi_dev* dev, const uint8_t* data, size_t len,uint32_t timeout_ms)
{
    MINI_IGNORE_RESULT(dev);
    MINI_IGNORE_RESULT(data);
    MINI_IGNORE_RESULT(len);
    MINI_IGNORE_RESULT(timeout_ms);
    return MINI_ERR_NOTSUPP;
}
