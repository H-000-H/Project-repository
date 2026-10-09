/* SPDX-License-Identifier: Apache-2.0 */
/*
 * UART HAL — STM32F1 实现 (LL 库)
 *
 * 设计: 上层接口 hal_uart.h 不变, 平台实现直接用 STM32 LL 库寄存器级函数。
 * - GPIO/时钟/DMA 配置在 hw_open 内一次性完成 (无 HAL MspInit 流程)。
 * - 上层通过 hal_uart_dev (ctlr=host) 调用, 实现内部用 static 池把 host 关联到 USART 实例。
 * - 轮询: LL_USART_TransmitData8 / LL_USART_ReceiveData8
 * - DMA : LL_DMA_* (F1 形态: DMA1 实例 + LL_DMA_CHANNEL_x) + LL_USART_EnableDMAReq_TX
 * - 不依赖 STM32 HAL 库 (stm32f1xx_hal.c 等), 仅依赖 LL 层头文件。
 *
 * 说明: F1 LL_DMA API 形态为 LL_DMA_Xxx(DMA_TypeDef *DMAx, uint32_t Channel, ...),
 *       通道用 LL_DMA_CHANNEL_x 枚举值, 不可用 DMA_Channel_TypeDef* 指针形式。
 */
#include "hal_uart.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_gpio.h"
#include "stm32f1xx_ll_usart.h"
#include "stm32f1xx_ll_dma.h"

#define HAL_UART_F1_INSTANCES 1U

typedef struct hal_uart_f1_ctx
{
    bool  used;
    struct hal_uart_bus_host* host;   /* 关联上层 host */
    USART_TypeDef*            usart;  /* UART 外设寄存器基址 */
    uint32_t                  dma_ch; /* TX DMA 通道 (LL_DMA_CHANNEL_x, 0=未用) */
    bool                      dma_tx_ok;
} hal_uart_f1_ctx_t;

static hal_uart_f1_ctx_t s_ctx[HAL_UART_F1_INSTANCES];

/** 按 USART 实例定位上下文 */
static hal_uart_f1_ctx_t* hal_uart_f1_find(USART_TypeDef* inst)
{
    uint32_t i;
    for (i = 0; i < HAL_UART_F1_INSTANCES; i++)
        if (s_ctx[i].used && s_ctx[i].usart == inst)
            return &s_ctx[i];
    return NULL;
}

/** 配置单个 UART 引脚 (F1 无 AFR) */
static void hal_uart_f1_config_pin(const struct hal_uart_pin_cfg* pin)
{
    LL_GPIO_InitTypeDef gi = {0};
    gi.Pin   = pin->pin;                              /* LL 位掩码 GPIO_PIN_x */
    gi.Mode  = pin->mode ? pin->mode : LL_GPIO_MODE_ALTERNATE;
    gi.Speed = pin->speed ? pin->speed : LL_GPIO_SPEED_FREQ_HIGH;
    gi.Pull  = pin->pull;
    LL_GPIO_Init((GPIO_TypeDef*)pin->port, &gi);
}

/** 按 GPIO 基址使能端口时钟 */
static void hal_uart_f1_enable_gpio_clock(uintptr_t port)
{
    if (port == GPIOA_BASE)      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    else if (port == GPIOB_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    else if (port == GPIOC_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    else if (port == GPIOD_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOD);
    else if (port == GPIOE_BASE) LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOE);
}

/** 按 UART 基址使能外设时钟 (USART1=APB2, USART2/3=APB1) */
static void hal_uart_f1_enable_uart_clock(uintptr_t uart_base)
{
    if (uart_base == USART1_BASE)      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
    else if (uart_base == USART2_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
    else if (uart_base == USART3_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART3);
}

/* =============================================================================
 * 上层接口 (hal_uart.h)
 * ============================================================================= */

int hal_uart_dev_init(struct hal_uart_bus_host* host, const struct hal_uart_config* cfg)
{
    if (!host || !cfg)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg  = *cfg;
    host->uart = cfg->uart;
    host->status = 0;
    return VFS_OK;
}

int hal_uart_dev_hw_open(struct hal_uart_bus_host* host)
{
    hal_uart_f1_ctx_t* ctx;
    uint32_t i;

    if (!host || !host->cfg.uart)
        return VFS_ERR_INVAL;
    if (host->hw_inited)
        return VFS_OK;

    /* 找/分配上下文并关联 host */
    ctx = hal_uart_f1_find((USART_TypeDef*)host->cfg.uart);
    if (!ctx)
    {
        for (i = 0; i < HAL_UART_F1_INSTANCES; i++)
        {
            if (!s_ctx[i].used) { ctx = &s_ctx[i]; break; }
        }
        if (!ctx)
            return VFS_ERR_NOMEM;
        COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
        ctx->used  = true;
        ctx->host  = host;
        ctx->usart = (USART_TypeDef*)host->cfg.uart;
        ctx->dma_ch = 0;
    }

    /* 时钟 */
    hal_uart_f1_enable_uart_clock((uintptr_t)ctx->usart);
    hal_uart_f1_enable_gpio_clock((uintptr_t)host->cfg.tx.port);
    hal_uart_f1_enable_gpio_clock((uintptr_t)host->cfg.rx.port);

    /* GPIO 复用: TX=复用推挽, RX=浮空输入 */
    hal_uart_f1_config_pin(&host->cfg.tx);
    hal_uart_f1_config_pin(&host->cfg.rx);

    /* DMA: dma_enable=1 且指定通道时初始化 TX DMA 通道 (F1: DMA1 + LL_DMA_CHANNEL_x)
     * 逐字段 LL_DMA_SetXxx 配置, 与 stm32_slice 参考工程官方硬编码写法一致 */
    ctx->dma_tx_ok = false;
    ctx->dma_ch = 0;
    if (host->cfg.dma_cfg.dma_enable && host->cfg.dma_cfg.dma_channel >= 1U &&
        host->cfg.dma_cfg.dma_channel <= 7U)
    {
        LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
        /* dtsi 给的 dma_channel 是 1..7, 映射到 F1 LL_DMA_CHANNEL_x (0x01..0x07) */
        static const uint32_t f1_ch[] =
        {
            LL_DMA_CHANNEL_1, LL_DMA_CHANNEL_2, LL_DMA_CHANNEL_3, LL_DMA_CHANNEL_4,
            LL_DMA_CHANNEL_5, LL_DMA_CHANNEL_6, LL_DMA_CHANNEL_7
        };
        uint32_t ch = f1_ch[host->cfg.dma_cfg.dma_channel - 1U];
        LL_DMA_DeInit(DMA1, ch);

        LL_DMA_SetDataTransferDirection(DMA1, ch, host->cfg.dma_cfg.dma_direction ?
                                                       host->cfg.dma_cfg.dma_direction : LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
        LL_DMA_SetChannelPriorityLevel(DMA1, ch, host->cfg.dma_cfg.dma_priority ?
                                                       host->cfg.dma_cfg.dma_priority : LL_DMA_PRIORITY_LOW);
        LL_DMA_SetMode(DMA1, ch, host->cfg.dma_cfg.dma_mode ?
                                       host->cfg.dma_cfg.dma_mode : LL_DMA_MODE_NORMAL);
        LL_DMA_SetPeriphIncMode(DMA1, ch, host->cfg.dma_cfg.dma_periph_inc ?
                                          host->cfg.dma_cfg.dma_periph_inc : LL_DMA_PERIPH_NOINCREMENT);
        LL_DMA_SetMemoryIncMode(DMA1, ch, host->cfg.dma_cfg.dma_mem_inc ?
                                          host->cfg.dma_cfg.dma_mem_inc : LL_DMA_MEMORY_INCREMENT);
        LL_DMA_SetPeriphSize(DMA1, ch, host->cfg.dma_cfg.dma_periph_data_size ?
                                        host->cfg.dma_cfg.dma_periph_data_size : LL_DMA_PDATAALIGN_BYTE);
        LL_DMA_SetMemorySize(DMA1, ch, host->cfg.dma_cfg.dma_memory_size ?
                                       host->cfg.dma_cfg.dma_memory_size : LL_DMA_MDATAALIGN_BYTE);
        /* 地址与长度在每次 write_dma 时通过 LL_DMA_ConfigAddresses/SetDataLength 设定 */
        ctx->dma_ch = ch;
        ctx->dma_tx_ok = true;
    }

    /* USART 参数 (hal_uart.h 字段已是 LL 宏值, 直接填入) */
    LL_USART_InitTypeDef ui = {0};
    ui.BaudRate            = host->cfg.baud_rate;
    ui.DataWidth           = host->cfg.data_width;
    ui.StopBits            = host->cfg.stop_bits;
    ui.Parity              = host->cfg.parity;
    ui.TransferDirection   = host->cfg.direction ? host->cfg.direction : LL_USART_DIRECTION_TX_RX;
    ui.HardwareFlowControl = host->cfg.hw_control;
    ui.OverSampling        = host->cfg.oversampling;
    LL_USART_Init(ctx->usart, &ui);
    LL_USART_Enable(ctx->usart);

    host->uart      = host->cfg.uart;
    host->hw_inited = true;
    host->status    = 1; /* READY */
    return VFS_OK;
}

int hal_uart_dev_hw_close(struct hal_uart_bus_host* host)
{
    hal_uart_f1_ctx_t* ctx;
    if (!host || !host->uart)
        return VFS_ERR_INVAL;

    ctx = hal_uart_f1_find((USART_TypeDef*)host->uart);
    if (ctx)
    {
        if (ctx->dma_tx_ok && ctx->dma_ch)
            LL_DMA_DeInit(DMA1, ctx->dma_ch);
        LL_USART_Disable(ctx->usart);
        ctx->used = false;
    }
    host->hw_inited = false;
    host->status    = 0;
    return VFS_OK;
}

int hal_uart_write(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    hal_uart_f1_ctx_t* ctx;
    struct hal_uart_bus_host* host;
    size_t n;
    uint32_t guard = (timeout_ms ? timeout_ms : 100U) * 4000U;

    if (!dev || !dev->ctlr || !data || len == 0)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    ctx  = hal_uart_f1_find((USART_TypeDef*)host->uart);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;

    host->status = 2; /* BUSY */
    for (n = 0; n < len; n++)
    {
        uint32_t g = 0;
        while (!LL_USART_IsActiveFlag_TXE(ctx->usart))
        {
            if (++g > guard) { host->status = 3; return VFS_ERR_TIMEOUT; }
        }
        LL_USART_TransmitData8(ctx->usart, data[n]);
    }
    uint32_t g = 0;
    while (!LL_USART_IsActiveFlag_TC(ctx->usart))
    {
        if (++g > guard) { host->status = 3; return VFS_ERR_TIMEOUT; }
    }
    LL_USART_ClearFlag_TC(ctx->usart);
    host->status = 1;
    return VFS_OK;
}

int hal_uart_read(struct hal_uart_dev* dev, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    hal_uart_f1_ctx_t* ctx;
    struct hal_uart_bus_host* host;
    size_t n;
    uint32_t guard = (timeout_ms ? timeout_ms : 100U) * 4000U;

    if (!dev || !dev->ctlr || !data || len == 0)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    ctx  = hal_uart_f1_find((USART_TypeDef*)host->uart);
    if (!ctx)
        return VFS_ERR_IO;
    if (len > 0xFFFFU)
        len = 0xFFFFU;

    host->status = 2; /* BUSY */
    for (n = 0; n < len; n++)
    {
        uint32_t g = 0;
        while (!LL_USART_IsActiveFlag_RXNE(ctx->usart))
        {
            if (++g > guard) { host->status = 3; return VFS_ERR_TIMEOUT; }
        }
        data[n] = (uint8_t)LL_USART_ReceiveData8(ctx->usart);
    }
    host->status = 1;
    return VFS_OK;
}

int hal_uart_write_dma(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    hal_uart_f1_ctx_t* ctx;
    struct hal_uart_bus_host* host;
    uint32_t guard;

    if (!dev || !dev->ctlr || !data || len == 0)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    ctx  = hal_uart_f1_find((USART_TypeDef*)host->uart);
    if (!ctx)
        return VFS_ERR_IO;
    if (!ctx->dma_tx_ok || !ctx->dma_ch)
        return VFS_ERR_NOTSUPP;
    if (len > 0xFFFFU)
        len = 0xFFFFU;

    host->status = 2;

    LL_USART_ClearFlag_TC(ctx->usart);
    LL_DMA_DisableChannel(DMA1, ctx->dma_ch);
    LL_DMA_SetDataLength(DMA1, ctx->dma_ch, (uint32_t)len);
    /* F1 LL_DMA_ConfigAddresses(DMAx, Channel, SrcAddress, DstAddress, Direction);
     * MEMORY_TO_PERIPH 时 SrcAddress 放 CMAR(内存源), DstAddress 放 CPAR(外设目的) */
    LL_DMA_ConfigAddresses(DMA1, ctx->dma_ch,
                           (uint32_t)data,             /* SrcAddress = 发送缓冲(内存源) */
                           (uint32_t)&ctx->usart->DR,  /* DstAddress = USART 数据寄存器(外设目的) */
                           LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_EnableChannel(DMA1, ctx->dma_ch);
    LL_USART_EnableDMAReq_TX(ctx->usart);

    guard = (timeout_ms ? timeout_ms : 10U) * 4000U;
    while (!LL_USART_IsActiveFlag_TC(ctx->usart))
    {
        if (--guard == 0U) { host->status = 3; return VFS_ERR_TIMEOUT; }
    }
    LL_USART_DisableDMAReq_TX(ctx->usart);
    LL_DMA_DisableChannel(DMA1, ctx->dma_ch);
    LL_USART_ClearFlag_TC(ctx->usart);
    host->status = 1;
    return VFS_OK;
}

int hal_uart_dma_abort(struct hal_uart_dev* dev)
{
    hal_uart_f1_ctx_t* ctx;
    struct hal_uart_bus_host* host;
    if (!dev || !dev->ctlr)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    ctx  = hal_uart_f1_find((USART_TypeDef*)host->uart);
    if (!ctx)
        return VFS_ERR_IO;

    LL_USART_DisableDMAReq_TX(ctx->usart);
    if (ctx->dma_tx_ok && ctx->dma_ch)
        LL_DMA_DisableChannel(DMA1, ctx->dma_ch);
    host->status = 1;
    return VFS_OK;
}
