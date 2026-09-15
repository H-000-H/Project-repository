/* SPDX-License-Identifier: Apache-2.0 */
/*
 * UART HAL — STM32F4 实现
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 LL 库。
 * - hal_uart_bus_host 嵌入 bus 层, HAL 无池管理无 vtable
 * - 自行配置 GPIO AF, 不依赖 CubeMX; write/read 用 LL_USART 轮询, fast path 访问 dev->uart
 */
#include "hal_uart.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_usart.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_dma.h"
#include "dt_config_gen.h"
#include "interrupt.h"

/** UART 下半部工作项 (fn/arg 由 VFS 层绑定), 供 interrupt_virtual_register 注册 */
struct bottom_half_work g_uart_bottom_half_work;

/* ── 平台参数：来自 DTS stm32,uart-platform-cap，无 DTS 时提供回退 ── */
#ifndef DTC_GEN_STM32_UART_MAX_XFER
#define DTC_GEN_STM32_UART_MAX_XFER  512U
#endif
#ifndef DTC_GEN_STM32_UART_TIMEOUT_MS
#define DTC_GEN_STM32_UART_TIMEOUT_MS  10U
#endif

#define STM32_UART_DMA_MAX_XFER    DTC_GEN_STM32_UART_MAX_XFER
#define STM32_UART_READ_TIMEOUT_MS DTC_GEN_STM32_UART_TIMEOUT_MS

/**
 * @brief 解析 UART 超时 (0 时使用平台默认)
 * @param timeout_ms 调用方超时 (ms)
 * @return 有效超时毫秒数
 */
MINI_STATIC_INLINE uint32_t stm32_uart_timeout(uint32_t timeout_ms)
{
    return timeout_ms ? timeout_ms : STM32_UART_READ_TIMEOUT_MS;
}

/**
 * @brief 由 UART 基址换算 VIRQ(uart, N) 的索引
 * @note  必须与 interrupt_stm32.c 里 USARTx_IRQHandler 的 dispatch 编号一致
 */
int hal_uart_virq_index(uintptr_t uart_base)
{
    switch (uart_base)
    {
    case USART1_BASE: return 0;
    case USART2_BASE: return 1;
    case USART3_BASE: return 2;
    case UART4_BASE:  return 3;
    case UART5_BASE:  return 4;
    case USART6_BASE: return 5;
    default:          return -1;
    }
}

/*============================================================================*/
/*                              LL 库直投 helper                              */
/*============================================================================*/
/* 纯 LL 库调用, 非抽象层 */
/**
 * @brief 配置 UART 复用引脚: 时钟使能 + AF 模式 + 推挽高速 (LL 库直投)
 * @param pin 引脚配置 (含 port/pin/clk_bus/af)
 */
MINI_STATIC_INLINE void hal_uart_config_af_pin(const struct hal_uart_pin_cfg* pin)
{
    GPIO_TypeDef* port = (GPIO_TypeDef*)pin->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin        = pin->pin;
    /* DTS 未写 mode(=0) 时默认为 AF, 避免 LL_GPIO_MODE_INPUT 导致复用失效 */
    GPIO_InitStruct.Mode       = pin->mode ? pin->mode : LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Pull       = pin->pull;
    GPIO_InitStruct.Alternate  = pin->af;
    GPIO_InitStruct.Speed      = pin->speed ? pin->speed : LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = pin->output_type;

    LL_AHB1_GRP1_EnableClock(pin->clk_bus);
    LL_GPIO_Init(port, &GPIO_InitStruct);
}

/**
 * @brief 按 UART 基址使能 APB1/APB2 外设时钟
 * @param uart_base UART 控制器基址 (USART1/USART6→APB2, 其余→APB1)
 * @param clk_periph LL 时钟外设掩码
 */
static void hal_uart_enable_periph_clock(uintptr_t uart_base, uint32_t clk_periph)
{
    if (uart_base == USART1_BASE || uart_base == USART6_BASE)
        LL_APB2_GRP1_EnableClock(clk_periph);
    else
        LL_APB1_GRP1_EnableClock(clk_periph);
}

/**
 * @brief 按 DMA 基址使能 AHB1 DMA1/DMA2 时钟
 * @param dma_base DMA 控制器基址 (DMA1_BASE 或 DMA2_BASE), 非法时跳过
 */
static void hal_uart_enable_dma_clock(uintptr_t dma_base)
{
    uint32_t periph = (dma_base == DMA1_BASE) ? LL_AHB1_GRP1_PERIPH_DMA1 : (dma_base == DMA2_BASE) ? LL_AHB1_GRP1_PERIPH_DMA2 : 0U;
    if (periph)
        LL_AHB1_GRP1_EnableClock(periph);
}

#define UART_OR_DEF(v, d) ((v) ? (v) : (d))

/**
 * @brief 轮询等待 UART TC (发送完成) 标志置位
 * @param usart      UART 外设寄存器基址
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int stm32_uart_wait_tc(USART_TypeDef* usart, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (!LL_USART_IsActiveFlag_TC(usart))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }
    return MINI_OK;
}

/*============================================================================*/
/*                              DMA helper                                    */
/*============================================================================*/
/**
 * @brief 清除 DMA stream 对应的 TC 标志 (STM32F4 LL 库按 stream 编号分布)
 * @param dma    DMA 控制器基址
 * @param stream DMA 流编号 (0..7)
 */
static void hal_uart_dma_clear_tc(DMA_TypeDef* dma, uint32_t stream)
{
    switch (stream)
    {
    case LL_DMA_STREAM_0: LL_DMA_ClearFlag_TC0(dma); break;
    case LL_DMA_STREAM_1: LL_DMA_ClearFlag_TC1(dma); break;
    case LL_DMA_STREAM_2: LL_DMA_ClearFlag_TC2(dma); break;
    case LL_DMA_STREAM_3: LL_DMA_ClearFlag_TC3(dma); break;
    case LL_DMA_STREAM_4: LL_DMA_ClearFlag_TC4(dma); break;
    case LL_DMA_STREAM_5: LL_DMA_ClearFlag_TC5(dma); break;
    case LL_DMA_STREAM_6: LL_DMA_ClearFlag_TC6(dma); break;
    case LL_DMA_STREAM_7: LL_DMA_ClearFlag_TC7(dma); break;
    default: break;
    }
}

/**
 * @brief DMA 静态参数一次性配置 (hw_open 时调用, 热路径不再重复)
 * @param cfg DMA 配置 (来自 DTS), 无效或 dma_handle 为空时跳过
 * @param usart_dr USART DR 寄存器地址 (periph 端固定地址)
 * @note  采用 LL_DMA_InitTypeDef + LL_DMA_Init 批量初始化范式 (同 LL_USART_Init/LL_ADC_Init)。
 *        channel/direction/priority/mode/inc/size 来自 DTS 且永不变;
 *        仅 buffer 地址与长度每次传输不同, 留在 write_dma 热路径。
 */
static void hal_uart_dma_init(const struct hal_uart_dma_config* cfg, uintptr_t usart_dr)
{
    LL_DMA_InitTypeDef init = {0};

    if (!cfg || !cfg->dma_handle)
        return;

    hal_uart_enable_dma_clock(cfg->dma_handle);

    /** STM32F4 要求配置前 stream disable */
    LL_DMA_DisableStream((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream);

    init.PeriphOrM2MSrcAddress   = usart_dr;
    init.MemoryOrM2MDstAddress   = 0;          /* 动态: write_dma 热路径设 */
    /* 短元组未填扩展字段时补默认 (0 对 mem_inc 非法) */
    init.Direction               = UART_OR_DEF(cfg->dma_direction, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    init.Mode                    = cfg->dma_mode; /* 0 == NORMAL */
    init.PeriphOrM2MSrcIncMode   = cfg->dma_periph_inc; /* 0 == NOINCREMENT */
    init.MemoryOrM2MDstIncMode   = UART_OR_DEF(cfg->dma_mem_inc, LL_DMA_MEMORY_INCREMENT);
    init.PeriphOrM2MSrcDataSize  = UART_OR_DEF(cfg->dma_periph_data_size, LL_DMA_PDATAALIGN_BYTE);
    init.MemoryOrM2MDstDataSize  = UART_OR_DEF(cfg->dma_memory_size, LL_DMA_MDATAALIGN_BYTE);
    init.NbData                  = 0;          /* 动态: write_dma 热路径设 */
    init.Channel                 = cfg->dma_channel;
    init.Priority                = cfg->dma_priority;
    init.FIFOMode                = cfg->dma_fifo_mode;
    init.FIFOThreshold           = cfg->dma_fifo_threshold;
    init.MemBurst                = cfg->dma_mem_burst;
    init.PeriphBurst             = cfg->dma_periph_burst;
    LL_DMA_Init((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream, &init);
}

/*============================================================================*/
/*                              Device 管理 API                               */
/*============================================================================*/
/**
 * @brief 初始化 UART host 对象 (拷贝配置, 缓存 uart 基址)
 * @param host UART 总线主机对象指针
 * @param cfg UART 配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
mt_err_t hal_uart_dev_init(struct hal_uart_bus_host* host, const struct hal_uart_config* cfg)
{
    if (!host || !cfg)
        return MINI_ERR_INVAL;

    __builtin_memset(host, 0, sizeof(*host));
    host->cfg    = *cfg;
    host->uart   = cfg->uart;
    host->status = 0;

    /* RX 环形缓冲句柄绑定到内嵌数据区 (item_size=1 即字节流) */
    if (fifo_uni_init(&host->rx_fifo, host->rx_buf, 1u, HAL_UART_RX_RING_SIZE) != BUFF_OK)
        return MINI_ERR_INVAL;
    return MINI_OK;
}

/**
 * @brief 打开 UART 硬件 (GPIO AF + LL_USART_Init + 可选 DMA 静态配置)
 * @param host UART 总线主机对象指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_IO
 */
mt_err_t hal_uart_dev_hw_open(struct hal_uart_bus_host* host)
{
    LL_USART_InitTypeDef init = {0};
    USART_TypeDef*       uart;

    if (!host || !host->cfg.uart)
        return MINI_ERR_INVAL;
    if (host->hw_inited)
        return MINI_OK;

    uart = (USART_TypeDef*)host->cfg.uart;
    hal_uart_enable_periph_clock(host->cfg.uart, host->cfg.uart_clk_periph);

    hal_uart_config_af_pin(&host->cfg.tx);
    hal_uart_config_af_pin(&host->cfg.rx);

    LL_USART_Disable(uart);

    init.BaudRate            = host->cfg.baud_rate;
    init.DataWidth           = host->cfg.data_width;
    init.StopBits            = host->cfg.stop_bits;
    init.Parity              = host->cfg.parity;
    /* DTS 未写 direction(=0) 时默认同收发, 避免 LL_USART_DIRECTION_NONE */
    init.TransferDirection   = host->cfg.direction ?
                               host->cfg.direction : LL_USART_DIRECTION_TX_RX;
    init.HardwareFlowControl = host->cfg.hw_control;
    init.OverSampling        = host->cfg.oversampling;

    if (LL_USART_Init(uart, &init) != SUCCESS)
        return MINI_ERR_IO;

    LL_USART_ConfigAsyncMode(uart);
    LL_USART_Enable(uart);

    /* 接收走中断 (it_enable=1): 本芯片 UART 无硬件 FIFO, 靠轮询读必然丢突发字节,
     * 中断逐字节收进环形缓冲后, hal_uart_read 只管按自己的节奏取 */
    if (host->cfg.it_enable)
        LL_USART_EnableIT_RXNE(uart);

    /** DMA 静态参数一次性配置: dma_enable=0 时跳过 */
    if (host->cfg.dma_cfg.dma_enable)
        hal_uart_dma_init(&host->cfg.dma_cfg, (uintptr_t)&uart->DR);

    /* 缓存 fast path 字段 */
    host->uart      = host->cfg.uart;
    host->hw_inited = true;
    host->status    = 1;  /* READY */
    return MINI_OK;
}

/**
 * @brief 关闭 UART 硬件 (LL_USART_Disable)
 * @param host UART 总线主机对象指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
mt_err_t hal_uart_dev_hw_close(struct hal_uart_bus_host* host)
{
    if (!host || !host->uart)
        return MINI_ERR_INVAL;

    LL_USART_Disable((USART_TypeDef*)host->uart);
    host->hw_inited = false;
    host->status    = 0;  /* UNINIT */
    return MINI_OK;
}

/*============================================================================*/
/*                              同步传输                                       */
/*============================================================================*/
/**
 * @brief UART 同步写: 配了 DMA 自动走 DMA, 否则 CPU 轮询
 * @param dev UART 设备指针
 * @param data 发送数据缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms, 0 用平台默认)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL / MINI_ERR_IO / MINI_ERR_TIMEOUT
 * @note 调用方只管 device_write, 不用关心底层是 DMA 还是 CPU 搬运 —— DTS 配了
 *       dma-enable 就自动走 DMA。长度超过 DMA 单次上限 (或 dma_enable=0) 时退回
 *       轮询, 所以大块数据仍然能发, 只是退化为 CPU 搬运。
 */
mt_err_t hal_uart_write(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct hal_uart_bus_host* host;
    USART_TypeDef*            usart;
    uint32_t                  start;
    uint32_t                  to;
    size_t                    i;

    if (!dev || !dev->ctlr || !data || len == 0)
        return MINI_ERR_INVAL;

    host  = dev->ctlr;

    /* 默认分流: DMA 可用且长度在单次上限内 → DMA (CPU 零搬运), 否则轮询。
     * 放在最前面, 让 write 成为唯一入口, 不必让上层区分两个 API。 */
    if (host->cfg.dma_cfg.dma_enable && len <= STM32_UART_DMA_MAX_XFER)
        return hal_uart_write_dma(dev, data, len, timeout_ms);

    usart = (USART_TypeDef*)host->uart;
    if (!usart)
        return MINI_ERR_IO;

    to = stm32_uart_timeout(timeout_ms);
    host->status = 2;  /* BUSY */
    start = HAL_GetTick();
    for (i = 0; i < len; i++)
    {
        while (!LL_USART_IsActiveFlag_TXE(usart))
        {
            if ((uint32_t)(HAL_GetTick() - start) >= to)
            {
                host->status = 3;  /* ERROR */
                return MINI_ERR_TIMEOUT;
            }
        }
        LL_USART_TransmitData8(usart, data[i]);
    }

    int ret = stm32_uart_wait_tc(usart, to);
    host->status = (ret == MINI_OK) ? (uint8_t)1 : (uint8_t)3;  /* READY : ERROR */
    return ret;
}

/**
 * @brief UART 轮询读 (RXNE)
 * @param dev UART 设备指针
 * @param data 接收缓冲
 * @param len 接收缓冲容量 (上限, 非"必须读满")
 * @param timeout_ms 超时 (ms, 0 用平台默认)
 * @return 实际读取字节数 (可能少于 len); 首字节就超时返回 MINI_ERR_TIMEOUT
 * @note  语义: 首字节等满 timeout_ms, 之后把当前已到位的字节一次收完就返回。
 *        不要求凑满 len —— 否则一次读会固定阻塞一个完整超时 (裸机主循环会被钉住)。
 */
mt_err_t hal_uart_read(struct hal_uart_dev* dev, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct hal_uart_bus_host* host;
    USART_TypeDef*            usart;
    uint32_t                  start;
    uint32_t                  to;
    size_t                    i;

    if (!dev || !dev->ctlr || !data || len == 0)
        return MINI_ERR_INVAL;

    host  = dev->ctlr;
    usart = (USART_TypeDef*)host->uart;
    if (!usart)
        return MINI_ERR_IO;

    to = stm32_uart_timeout(timeout_ms);
    host->status = 2;  /* BUSY */
    start = HAL_GetTick();

    /* 中断接收路径: 数据已由 ISR 收进环形缓冲, 与调用方的读节奏解耦,
     * 因此不会再出现"读一次只拿到 1 个字节"的丢包 */
    if (LL_USART_IsEnabledIT_RXNE(usart))
    {
        uint16_t count = 0u;
        uint16_t got   = 0u;

        /* 首字节: 唯一的阻塞点, 缓冲为空时最多等到 timeout_ms */
        fifo_uni_get_count(&host->rx_fifo, &count);
        while (count == 0u)
        {
            if ((uint32_t)(HAL_GetTick() - start) >= to)
            {
                host->status = 3;  /* ERROR */
                return MINI_ERR_TIMEOUT;
            }
            fifo_uni_get_count(&host->rx_fifo, &count);
        }

        /* 后续字节: 有多少取多少, 不在帧尾空等满 len (与轮询路径语义一致) */
        fifo_uni_read_block(&host->rx_fifo, data, (uint16_t)len, &got);

        host->status = 1;  /* READY */
        return (int)got;
    }

    /* 未开中断 (it_enable=0): 退回原轮询路径 */
    /* 首字节: 唯一的阻塞点, 无数据时最多等到 timeout_ms */
    while (!LL_USART_IsActiveFlag_RXNE(usart))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= to)
        {
            host->status = 3;  /* ERROR */
            return MINI_ERR_TIMEOUT;
        }
    }
    data[0] = LL_USART_ReceiveData8(usart);

    /* 后续字节: 有多少收多少, 不在帧尾空等满 len */
    for (i = 1; (i < len) && LL_USART_IsActiveFlag_RXNE(usart); i++)
        data[i] = LL_USART_ReceiveData8(usart);

    host->status = 1;  /* READY */
    return (int)i;
}

/**
 * @brief UART DMA 写 (等 TC 完成)
 * @param dev UART 设备指针
 * @param data 发送数据缓冲
 * @param len 字节数 (不超过平台 DMA 上限)
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
mt_err_t hal_uart_write_dma(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct hal_uart_bus_host* host;
    USART_TypeDef*            usart;
    DMA_TypeDef*              dma;
    uint32_t                  stream;
    uint32_t                  dir;
    int                       ret;

    if (!dev || !dev->ctlr || !data || len == 0 || len > STM32_UART_DMA_MAX_XFER)
        return MINI_ERR_INVAL;

    host = dev->ctlr;
    if (!host->cfg.dma_cfg.dma_enable)
        return MINI_ERR_NOTSUPP;

    usart  = (USART_TypeDef*)host->uart;
    dma    = (DMA_TypeDef*)host->cfg.dma_cfg.dma_handle;
    stream = host->cfg.dma_cfg.dma_stream;
    if (!usart || !dma)
        return MINI_ERR_IO;

    dir = UART_OR_DEF(host->cfg.dma_cfg.dma_direction, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);

    LL_DMA_DisableStream(dma, stream);
    LL_DMA_ConfigAddresses(dma, stream, (uint32_t)data, (uint32_t)&usart->DR, dir);
    LL_DMA_SetDataLength(dma, stream, len);
    hal_uart_dma_clear_tc(dma, stream);
    LL_USART_ClearFlag_TC(usart);
    LL_USART_EnableDMAReq_TX(usart);
    LL_DMA_EnableStream(dma, stream);

    ret = stm32_uart_wait_tc(usart, stm32_uart_timeout(timeout_ms));
    if (ret != MINI_OK)
    {
        LL_DMA_DisableStream(dma, stream);
        LL_USART_DisableDMAReq_TX(usart);
        return ret;
    }

    hal_uart_dma_clear_tc(dma, stream);
    LL_DMA_DisableStream(dma, stream);
    LL_USART_DisableDMAReq_TX(usart);
    return MINI_OK;
}

/**
 * @brief 中止 UART DMA 发送
 * @param dev UART 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_IO
 */
mt_err_t hal_uart_dma_abort(struct hal_uart_dev* dev)
{
    struct hal_uart_bus_host* host;
    USART_TypeDef*            usart;
    DMA_TypeDef*              dma;

    if (!dev || !dev->ctlr)
        return MINI_ERR_INVAL;

    host  = dev->ctlr;
    usart = (USART_TypeDef*)host->uart;
    if (!usart)
        return MINI_ERR_IO;

    LL_USART_DisableDMAReq_TX(usart);

    if (host->cfg.dma_cfg.dma_enable && host->cfg.dma_cfg.dma_handle)
    {
        dma = (DMA_TypeDef*)host->cfg.dma_cfg.dma_handle;
        LL_DMA_DisableStream(dma, host->cfg.dma_cfg.dma_stream);
    }
    return MINI_OK;
}

/* =========================================================================================================================================================== */
/* ISR 虚拟中断回调                                                                                                                                              */
/* =========================================================================================================================================================== */

/**
 * @brief UART 虚拟中断上半部回调 (ISR 内执行)
 * @param arg UART 设备指针 (hal_uart_dev*)
 * @param irq_num 虚拟中断号
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部; MINI_IRQ_ENTRY_NOBOTTOM 不需要
 */
int hal_virtual_uart_irq_callback(void* arg, uint16_t irq_num)
{
    MINI_IGNORE_RESULT(irq_num);
    struct hal_uart_bus_host* host = (struct hal_uart_bus_host*)arg;

    if (!host || !host->uart)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    USART_TypeDef* usart = (USART_TypeDef*)host->uart;

    /* RXNE: 把 DR 里的字节收进环形缓冲。循环是为了兜住"中断被延迟"时
     * DR 之外可能已积压的字节; 上限取半个缓冲, 避免极端情况下 ISR 长占不放 */
    if (LL_USART_IsEnabledIT_RXNE(usart))
    {
        uint32_t guard = HAL_UART_RX_RING_SIZE / 2u;

        while ((guard-- > 0u) && LL_USART_IsActiveFlag_RXNE(usart))
        {
            const uint8_t byte = (uint8_t)LL_USART_ReceiveData8(usart);

            if (fifo_uni_write_block(&host->rx_fifo, &byte, 1u, NULL) != BUFF_OK)
                host->rx_dropped++; /* 满: 丢新字节, 保住已有数据好让上层拼出完整帧 */
        }
    }

    /* DMA TC: 清除标志 (仅 DMA 模式) */
    if (host->cfg.dma_cfg.dma_enable)
    {
        DMA_TypeDef* dma = (DMA_TypeDef*)host->cfg.dma_cfg.dma_handle;

        if (dma)
            hal_uart_dma_clear_tc(dma, host->cfg.dma_cfg.dma_stream);
        LL_USART_ClearFlag_TC(usart);
    }

    /* 收下的数据已在缓冲里, 不需要下半部 */
    return MINI_IRQ_ENTRY_NOBOTTOM;
}
