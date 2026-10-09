/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_uart_ch32v30x.c
 *@brief CH32V307 UART HAL 强符号实现 (覆盖 mini_tree/hal/uart/hal_uart.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头。
 *   直投约定 (对齐 hal_uart.h WCH 注释):
 *   - uart = USARTx_BASE, uart_clk_periph = RCC_APBxPeriph_USARTx;
 *   - 引脚 cfg: af 字段承载 GPIOMode_TypeDef (如 GPIO_Mode_AF_PP),
 *     mode 非 0 时优先用 mode, 否则回退 af; rx 缺省输入上拉;
 *   - data_width / parity / stop_bits / hw_control 直投 WCH USART_* 宏 (0=8N1/无流控);
 *   - 同步收发走 CPU polling (超时按 SystemCoreClock 折算);
 *     DMA 发送走 DMA1 固定映射通道 (USART1/2/3 TX = CH4/CH7/CH2),
 *     USART4..8 映射在 DMA2 未佐证, 返回 MINI_ERR_NOTSUPP;
 *     中断路径本板未接线。
 */

#include "ch32v30x_hal_common.h"
#include "hal_uart.h"

/* =============================================================================
 * 内部辅助 (首次打开一次性完成时钟/引脚/外设初始化)
 * ============================================================================= */

/**
 * @brief 配置并初始化 UART 硬件 (首次 hw_open 调用)
 * @param[in] host UART 主机上下文 (cfg 已绑定)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
static int s_uart_hw_setup(struct hal_uart_bus_host* host)
{
    const struct hal_uart_config* cfg = &host->cfg;
    USART_TypeDef* usart = (USART_TypeDef*)cfg->uart;
    if (usart == NULL)
        return MINI_ERR_INVAL;

    int ret = ch32_rcc_enable(cfg->uart, cfg->uart_clk_periph);
    if (ret != MINI_OK)
        return ret;

    /* TX: 缺省复用推挽输出 */
    if (cfg->tx.port != 0U)
    {
        ret = ch32_gpio_port_clk(cfg->tx.port, cfg->tx.clk_bus);
        if (ret != MINI_OK)
            return ret;
        uint32_t tx_mode = (cfg->tx.mode != 0U) ? cfg->tx.mode : cfg->tx.af;
        if (tx_mode == 0U)
            tx_mode = (uint32_t)GPIO_Mode_AF_PP;
        ret = ch32_gpio_cfg_pin(cfg->tx.port, ch32_pin_to_mask(cfg->tx.pin), tx_mode, cfg->tx.speed,
                                cfg->tx.pull);
        if (ret != MINI_OK)
            return ret;
    }

    /* RX: 缺省输入上拉 */
    if (cfg->rx.port != 0U)
    {
        ret = ch32_gpio_port_clk(cfg->rx.port, cfg->rx.clk_bus);
        if (ret != MINI_OK)
            return ret;
        uint32_t rx_mode = (cfg->rx.mode != 0U) ? cfg->rx.mode : cfg->rx.af;
        if (rx_mode == 0U)
            rx_mode = (uint32_t)GPIO_Mode_IPU;
        uint32_t rx_pull = (cfg->rx.pull != 0U) ? cfg->rx.pull : 0x04U;
        ret = ch32_gpio_cfg_pin(cfg->rx.port, ch32_pin_to_mask(cfg->rx.pin), rx_mode, cfg->rx.speed,
                                rx_pull);
        if (ret != MINI_OK)
            return ret;
    }

    USART_InitTypeDef ui;
    ui.USART_BaudRate = cfg->baud_rate;
    ui.USART_WordLength = (cfg->data_width != 0U) ? (uint16_t)cfg->data_width : USART_WordLength_8b;
    ui.USART_StopBits = (cfg->stop_bits != 0U) ? (uint16_t)cfg->stop_bits : USART_StopBits_1;
    ui.USART_Parity = (cfg->parity != 0U) ? (uint16_t)cfg->parity : USART_Parity_No;
    ui.USART_HardwareFlowControl =
        (cfg->hw_control != 0U) ? (uint16_t)cfg->hw_control : USART_HardwareFlowControl_None;
    ui.USART_Mode = (cfg->direction != 0U) ? (uint16_t)cfg->direction :
                                             (uint16_t)(USART_Mode_Tx | USART_Mode_Rx);
    USART_Init(usart, &ui);
    USART_Cmd(usart, ENABLE);

    host->uart = cfg->uart;
    host->hw_inited = true;
    host->status = 1;
    return MINI_OK;
}

/* =============================================================================
 * DMA 发送 (DMA1 固定映射: USART1/2/3 TX = CH4/CH7/CH2, 一次性搬运轮询等完成)
 * ============================================================================= */

/**
 * @brief USART 基址 → DMA1 发送通道 (固定映射查表)
 * @param[in] uart_base USARTx_BASE
 * @return DMA 通道指针; USART4..8 请求位于 DMA2 映射未佐证, 返回 NULL
 */
static DMA_Channel_TypeDef* s_uart_dma_tx_ch(uintptr_t uart_base)
{
    switch (uart_base)
    {
    case USART1_BASE:
        return DMA1_Channel4;
    case USART2_BASE:
        return DMA1_Channel7;
    case USART3_BASE:
        return DMA1_Channel2;
    default:
        return NULL;
    }
}

/**
 * @brief 停止 DMA 通道 (去使能 + 清全部标志)
 * @param[in] ch DMA 通道指针 (NULL 安全)
 */
static void s_uart_dma_stop(DMA_Channel_TypeDef* ch)
{
    if (ch == NULL)
        return;
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    DMA_Cmd(ch, DISABLE);
    DMA_ClearFlag(0xFU << ((idx - 1U) * 4U)); /* GL/TC/HT/TE 全清 */
}

/**
 * @brief 配置 TX 通道并启动一次性搬运 (内存→DATAR), 轮询等待完成 (超时/错误即停)
 * @param[in] ch DMA 通道指针 (s_uart_dma_tx_ch 返回值, 非空)
 * @param[in] periph_addr 外设数据寄存器地址 (&USARTx->DATAR)
 * @param[in] mem_addr 发送缓冲区地址 (DMA 期间须保持有效)
 * @param[in] len 搬运字节数 (1..65535)
 * @param[in] timeout_ms 等待完成的超时毫秒数 (按 SystemCoreClock 折算)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT,
 *         传输错误返回 MINI_ERR_IO, 参数无效返回 MINI_ERR_INVAL
 */
static int s_uart_dma_xfer(DMA_Channel_TypeDef* ch, uint32_t periph_addr, uint32_t mem_addr,
                           uint32_t len, uint32_t timeout_ms)
{
    if ((len == 0U) || (len > 0xFFFFU))
        return MINI_ERR_INVAL;
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    uint32_t shift = (idx - 1U) * 4U;
    const uint32_t tc = 1U << (shift + 1U);
    const uint32_t te = 1U << (shift + 3U);

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    DMA_Cmd(ch, DISABLE);

    DMA_InitTypeDef di;
    di.DMA_PeripheralBaseAddr = periph_addr;
    di.DMA_MemoryBaseAddr = mem_addr;
    di.DMA_DIR = DMA_DIR_PeripheralDST;
    di.DMA_BufferSize = len;
    di.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    di.DMA_MemoryInc = DMA_MemoryInc_Enable;
    di.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    di.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    di.DMA_Mode = DMA_Mode_Normal;
    di.DMA_Priority = DMA_Priority_High;
    di.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(ch, &di);

    DMA_ClearFlag(tc);
    DMA_ClearFlag(te);
    DMA_Cmd(ch, ENABLE);

    uint32_t budget = ch32_poll_budget(timeout_ms);
    while (DMA_GetFlagStatus(tc) == RESET)
    {
        if (DMA_GetFlagStatus(te) != RESET)
        {
            s_uart_dma_stop(ch);
            return MINI_ERR_IO;
        }
        if (budget == 0U)
        {
            s_uart_dma_stop(ch);
            return MINI_ERR_TIMEOUT;
        }
        budget--;
    }
    s_uart_dma_stop(ch);
    return MINI_OK;
}

/* =============================================================================
 * 上层接口 (hal_uart.h)
 * ============================================================================= */

/**
 * @brief 绑定 UART 主机配置 (不碰硬件, 首次打开时才初始化)
 * @param[in] host UART 主机上下文指针
 * @param[in] cfg UART 配置指针 (拷贝至 host->cfg)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_uart_dev_init(struct hal_uart_bus_host* host, const struct hal_uart_config* cfg)
{
    if ((host == NULL) || (cfg == NULL))
        return MINI_ERR_INVAL;
    host->cfg = *cfg;
    host->uart = cfg->uart;
    host->uart_queue = NULL;
    host->status = 0;
    host->hw_inited = false;
    return MINI_OK;
}

/**
 * @brief 打开 UART 硬件 (首次调用执行时钟/引脚/参数配置并使能外设)
 * @param[in] host UART 主机上下文指针
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_uart_dev_hw_open(struct hal_uart_bus_host* host)
{
    if (host == NULL)
        return MINI_ERR_INVAL;
    if (!host->hw_inited)
        return s_uart_hw_setup(host);
    return MINI_OK;
}

/**
 * @brief 关闭 UART 硬件 (去使能外设, 下次打开始重新初始化)
 * @param[in] host UART 主机上下文指针
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_uart_dev_hw_close(struct hal_uart_bus_host* host)
{
    if (host == NULL)
        return MINI_ERR_INVAL;
    if (host->hw_inited)
    {
        USART_TypeDef* usart = (USART_TypeDef*)host->uart;
        USART_Cmd(usart, DISABLE);
        host->hw_inited = false;
        host->status = 0;
    }
    return MINI_OK;
}

/**
 * @brief 同步发送 (逐字节等 TXE, 末尾等 TC 移出移位寄存器)
 * @param[in] pdev UART 设备指针
 * @param[in] data 发送数据缓冲区 (允许空指针配合 len=0)
 * @param[in] len 发送字节数
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, 参数无效返回 MINI_ERR_INVAL
 */
int hal_uart_write(struct hal_uart_dev* pdev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((data == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    USART_TypeDef* usart = (USART_TypeDef*)pdev->ctlr->uart;
    uint32_t budget = ch32_poll_budget(timeout_ms);

    for (size_t i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(usart, USART_FLAG_TXE) == RESET)
        {
            if (budget == 0U)
                return MINI_ERR_TIMEOUT;
            budget--;
        }
        USART_SendData(usart, data[i]);
    }

    /* 等待最后一字节移出移位寄存器 */
    while (USART_GetFlagStatus(usart, USART_FLAG_TC) == RESET)
    {
        if (budget == 0U)
            return MINI_ERR_TIMEOUT;
        budget--;
    }
    return MINI_OK;
}

/**
 * @brief 同步接收 (逐字节等 RXNE)
 * @param[in] pdev UART 设备指针
 * @param[out] data 接收缓冲区 (允许空指针配合 len=0)
 * @param[in] len 接收字节数
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, 参数无效返回 MINI_ERR_INVAL
 */
int hal_uart_read(struct hal_uart_dev* pdev, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((data == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    USART_TypeDef* usart = (USART_TypeDef*)pdev->ctlr->uart;
    uint32_t budget = ch32_poll_budget(timeout_ms);

    for (size_t i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(usart, USART_FLAG_RXNE) == RESET)
        {
            if (budget == 0U)
                return MINI_ERR_TIMEOUT;
            budget--;
        }
        data[i] = (uint8_t)USART_ReceiveData(usart);
    }
    return MINI_OK;
}

/**
 * @brief DMA 发送 (内存→DATAR 一次性搬运, 轮询等 TC 后再等 TC 标志移出移位寄存器)
 * @param[in] pdev UART 设备指针 (必须已绑定总线并打开)
 * @param[in] data 发送缓冲区 (DMA 期间须保持有效)
 * @param[in] len 发送字节数 (0 = 空操作)
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT,
 *         外设无 DMA1 映射返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_uart_write_dma(struct hal_uart_dev* pdev, const uint8_t* data, size_t len,
                       uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((data == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    if (len == 0U)
        return MINI_OK;
    USART_TypeDef* usart = (USART_TypeDef*)pdev->ctlr->uart;
    DMA_Channel_TypeDef* ch = s_uart_dma_tx_ch(pdev->ctlr->uart);
    if (ch == NULL)
        return MINI_ERR_NOTSUPP;

    USART_DMACmd(usart, USART_DMAReq_Tx, ENABLE);
    int ret =
        s_uart_dma_xfer(ch, (uint32_t)&usart->DATAR, (uint32_t)data, (uint32_t)len, timeout_ms);
    USART_DMACmd(usart, USART_DMAReq_Tx, DISABLE);
    if (ret != MINI_OK)
        return ret;

    /* 等待最后一字节移出移位寄存器 */
    uint32_t budget = ch32_poll_budget(timeout_ms);
    while (USART_GetFlagStatus(usart, USART_FLAG_TC) == RESET)
    {
        if (budget == 0U)
            return MINI_ERR_TIMEOUT;
        budget--;
    }
    return MINI_OK;
}

/**
 * @brief 中止进行中的 DMA 传输 (去使能 TX DMA 请求与通道)
 * @param[in] pdev UART 设备指针 (必须已绑定总线)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_uart_dma_abort(struct hal_uart_dev* pdev)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    USART_TypeDef* usart = (USART_TypeDef*)pdev->ctlr->uart;
    USART_DMACmd(usart, USART_DMAReq_Tx, DISABLE);
    s_uart_dma_stop(s_uart_dma_tx_ch(pdev->ctlr->uart));
    return MINI_OK;
}
