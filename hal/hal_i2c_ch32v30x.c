/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_i2c_ch32v30x.c
 *@brief CH32V307 I2C HAL 强符号实现 (覆盖 mini_tree/hal/i2c/hal_i2c.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头。
 *   直投约定 (对齐 hal_i2c.h):
 *   - i2c = I2Cx_BASE, i2c_clk_periph = RCC_APB1Periph_I2Cx;
 *   - SCL/SDA 缺省复用开漏输出 50MHz (WCH F1 型 I2C 引脚);
 *   - device cfg: clock_speed_hz / duty_cycle / own_address 直投 WCH 宏,
 *     address 为 7-bit 右对齐 (传输时内部左移);
 *   - 主机同步收发走 CPU polling 状态机 (超时按 SystemCoreClock 折算);
 *   - DMA 路径: DMA1 固定映射 (I2C1 TX/RX = CH6/CH7, I2C2 TX/RX = CH2/CH3),
 *     传输耗时随长度增长, 超时预算按字节数/速率扩容;
 *     10-bit 地址未实现。
 */

#include "ch32v30x_hal_common.h"
#include "hal_i2c.h"

/**
 * @brief 等待指定标志到达目标电平 (消耗超时预算)
 * @param[in] i2c I2C 外设寄存器指针 (出参, 仅读标志)
 * @param[in] flag I2C_FLAG_* 标志位 (直投)
 * @param[in] want 目标电平 (SET/RESET)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int s_wait_flag(I2C_TypeDef* i2c, uint32_t flag, FlagStatus want, uint32_t* budget)
{
    while (I2C_GetFlagStatus(i2c, flag) != want)
    {
        if (*budget == 0U)
            return MINI_ERR_TIMEOUT;
        (*budget)--;
    }
    return MINI_OK;
}

/**
 * @brief 应用 client 配置到 I2C 外设 (首次打开或速率变更时)
 * @param[in] host I2C 主机上下文 (出参, 写入 active_cfg/状态)
 * @param[in] cfg 目标设备配置 (速率/地址与 active_cfg 一致且已就绪则跳过)
 * @return 成功返回 MINI_OK, 10-bit 地址返回 MINI_ERR_NOTSUPP
 */
static int s_i2c_apply_dev_cfg(struct hal_i2c_bus_host* host,
                               const struct hal_i2c_device_config* cfg)
{
    if ((host->bus_ready) && (host->active_cfg.clock_speed_hz == cfg->clock_speed_hz) &&
        (host->active_cfg.address == cfg->address))
    {
        return MINI_OK;
    }
    if (cfg->addr_width != 0U)
        return MINI_ERR_NOTSUPP;

    I2C_TypeDef* i2c = (I2C_TypeDef*)host->i2c;
    I2C_Cmd(i2c, DISABLE);

    I2C_InitTypeDef ii;
    ii.I2C_ClockSpeed = (cfg->clock_speed_hz != 0U) ? cfg->clock_speed_hz : 100000U;
    ii.I2C_Mode = (host->cfg.mode != 0U) ? (uint16_t)host->cfg.mode : I2C_Mode_I2C;
    ii.I2C_DutyCycle = (cfg->duty_cycle != 0U) ? (uint16_t)cfg->duty_cycle : I2C_DutyCycle_2;
    ii.I2C_OwnAddress1 = (uint16_t)cfg->own_address;
    ii.I2C_Ack = I2C_Ack_Enable;
    ii.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(i2c, &ii);
    I2C_Cmd(i2c, ENABLE);

    host->active_cfg = *cfg;
    host->bus_ready = true;
    return MINI_OK;
}

/**
 * @brief 主机写段 (START + addr + data... + 可选 STOP, NACK 检测返回 IO 错误)
 * @param[in] i2c I2C 外设寄存器指针 (出参, 仅写寄存器)
 * @param[in] addr_byte 7-bit 地址左移后的地址字节 (含读写位)
 * @param[in] tx 发送缓冲区首地址 (出参, 仅读)
 * @param[in] len 发送字节数 (出参)
 * @param[in] send_stop 是否发送 STOP (先写后读时写段发 START 不发 STOP)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT
 */
static int s_master_write(I2C_TypeDef* i2c, uint8_t addr_byte, const uint8_t* tx, size_t len,
                          bool send_stop, uint32_t* budget)
{
    int ret = s_wait_flag(i2c, I2C_FLAG_BUSY, RESET, budget);
    if (ret != MINI_OK)
        return ret;
    I2C_GenerateSTART(i2c, ENABLE);
    ret = s_wait_flag(i2c, I2C_FLAG_SB, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    I2C_Send7bitAddress(i2c, addr_byte, I2C_Direction_Transmitter);
    ret = s_wait_flag(i2c, I2C_FLAG_ADDR, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    (void)s_wait_flag(i2c, I2C_FLAG_ADDR, RESET, budget); /* 读 SR2 清 ADDR */

    for (size_t i = 0; i < len; i++)
    {
        ret = s_wait_flag(i2c, I2C_FLAG_TXE, SET, budget);
        if (ret != MINI_OK)
            goto s_fail;
        I2C_SendData(i2c, tx[i]);
    }
    ret = s_wait_flag(i2c, I2C_FLAG_BTF, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    if (I2C_GetFlagStatus(i2c, I2C_FLAG_AF) == SET)
    {
        /* 从机 NACK */
        I2C_ClearFlag(i2c, I2C_FLAG_AF);
        I2C_GenerateSTOP(i2c, ENABLE);
        return MINI_ERR_IO;
    }
    if (send_stop)
        I2C_GenerateSTOP(i2c, ENABLE);
    return MINI_OK;

s_fail:
    I2C_GenerateSTOP(i2c, ENABLE);
    return (ret == MINI_OK) ? MINI_ERR_IO : ret;
}

/**
 * @brief 主机读段 (可重复 START + addr + 收 len 字节, 最后一字节 NACK + STOP)
 * @param[in] i2c I2C 外设寄存器指针 (出参, 仅读数据寄存器)
 * @param[in] addr_byte 7-bit 地址左移后的地址字节 (含读写位)
 * @param[out] rx 接收缓冲区首地址 (出参, 写入接收数据)
 * @param[in] len 接收字节数 (出参)
 * @param[in] repeated_start 是否重复 START (先写后读时写段已发过地址)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, IO 错误返回 MINI_ERR_IO
 */
static int s_master_read(I2C_TypeDef* i2c, uint8_t addr_byte, uint8_t* rx, size_t len,
                         bool repeated_start, uint32_t* budget)
{
    if (len == 0U)
        return MINI_OK;
    int ret;
    if (!repeated_start)
    {
        ret = s_wait_flag(i2c, I2C_FLAG_BUSY, RESET, budget);
        if (ret != MINI_OK)
            return ret;
    }
    I2C_AcknowledgeConfig(i2c, ENABLE);
    I2C_GenerateSTART(i2c, ENABLE);
    ret = s_wait_flag(i2c, I2C_FLAG_SB, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    I2C_Send7bitAddress(i2c, addr_byte, I2C_Direction_Receiver);
    ret = s_wait_flag(i2c, I2C_FLAG_ADDR, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    if (len == 1U)
        I2C_AcknowledgeConfig(i2c, DISABLE); /* 单字节读: 清 ADDR 前先关 ACK */
    (void)s_wait_flag(i2c, I2C_FLAG_ADDR, RESET, budget); /* 读 SR2 清 ADDR */

    for (size_t i = 0; i < len; i++)
    {
        if ((i == (len - 1U)) && (len > 1U))
            I2C_AcknowledgeConfig(i2c, DISABLE);
        ret = s_wait_flag(i2c, I2C_FLAG_RXNE, SET, budget);
        if (ret != MINI_OK)
            goto s_fail;
        rx[i] = (uint8_t)I2C_ReceiveData(i2c);
    }
    I2C_GenerateSTOP(i2c, ENABLE);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    return MINI_OK;

s_fail:
    I2C_GenerateSTOP(i2c, ENABLE);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    return (ret == MINI_OK) ? MINI_ERR_IO : ret;
}

/* =============================================================================
 * DMA 收发 (DMA1 固定映射: I2C1 TX/RX=CH6/CH7, I2C2 TX/RX=CH2/CH3)
 * ============================================================================= */

/**
 * @brief I2C 基址 → DMA1 收发通道 (固定映射查表)
 * @param[in] i2c_base I2Cx_BASE
 * @param[out] tx_ch 发送通道输出指针 (内存→外设)
 * @param[out] rx_ch 接收通道输出指针 (外设→内存)
 * @return 成功返回 MINI_OK, 无映射返回 MINI_ERR_NOTSUPP
 */
static int s_i2c_dma_channels(uintptr_t i2c_base, DMA_Channel_TypeDef** tx_ch,
                              DMA_Channel_TypeDef** rx_ch)
{
    switch (i2c_base)
    {
    case I2C1_BASE:
        *tx_ch = DMA1_Channel6;
        *rx_ch = DMA1_Channel7;
        return MINI_OK;
    case I2C2_BASE:
        *tx_ch = DMA1_Channel2;
        *rx_ch = DMA1_Channel3;
        return MINI_OK;
    default:
        *tx_ch = NULL;
        *rx_ch = NULL;
        return MINI_ERR_NOTSUPP;
    }
}

/**
 * @brief 停止 DMA 通道 (去使能 + 清全部标志)
 * @param[in] ch DMA 通道指针 (NULL 安全)
 */
static void s_i2c_dma_stop(DMA_Channel_TypeDef* ch)
{
    if (ch == NULL)
        return;
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    DMA_Cmd(ch, DISABLE);
    DMA_ClearFlag(0xFU << ((idx - 1U) * 4U)); /* GL/TC/HT/TE 全清 */
}

/**
 * @brief 配置并启动一个 DMA 通道 (字节宽度, 内存自增, 不等待)
 * @param[in] ch DMA 通道指针 (出参, 写配置寄存器)
 * @param[in] periph_addr 外设数据寄存器地址 (&I2Cx->DATAR)
 * @param[in] mem_addr 内存缓冲区地址 (DMA 期间须保持有效)
 * @param[in] units 搬运字节数 (1..65535)
 * @param[in] dir DMA_DIR_PeripheralDST / DMA_DIR_PeripheralSRC (直投)
 */
static void s_i2c_dma_arm(DMA_Channel_TypeDef* ch, uint32_t periph_addr, uint32_t mem_addr,
                          uint32_t units, uint32_t dir)
{
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;

    DMA_Cmd(ch, DISABLE);

    DMA_InitTypeDef di;
    di.DMA_PeripheralBaseAddr = periph_addr;
    di.DMA_MemoryBaseAddr = mem_addr;
    di.DMA_DIR = dir;
    di.DMA_BufferSize = units;
    di.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    di.DMA_MemoryInc = DMA_MemoryInc_Enable;
    di.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    di.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    di.DMA_Mode = DMA_Mode_Normal;
    di.DMA_Priority = DMA_Priority_High;
    di.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(ch, &di);

    DMA_ClearFlag(1U << (((idx - 1U) * 4U) + 1U)); /* TC */
    DMA_ClearFlag(1U << (((idx - 1U) * 4U) + 3U)); /* TE */
    DMA_Cmd(ch, ENABLE);
}

/**
 * @brief 轮询等待通道传输完成 (消耗超时预算, 中途检测传输错误)
 * @param[in] ch DMA 通道指针 (仅读标志)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 传输错误返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT
 */
static int s_i2c_dma_wait_tc(DMA_Channel_TypeDef* ch, uint32_t* budget)
{
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    uint32_t shift = (idx - 1U) * 4U;
    const uint32_t tc = 1U << (shift + 1U);
    const uint32_t te = 1U << (shift + 3U);
    while (DMA_GetFlagStatus(tc) == RESET)
    {
        if (DMA_GetFlagStatus(te) != RESET)
            return MINI_ERR_IO;
        if (*budget == 0U)
            return MINI_ERR_TIMEOUT;
        (*budget)--;
    }
    return MINI_OK;
}

/**
 * @brief 主机 DMA 写段 (START + addr + DMA 搬运 + 可选 STOP, NACK 检测返回 IO 错误)
 * @param[in] i2c I2C 外设寄存器指针 (出参, 写寄存器)
 * @param[in] addr_byte 7-bit 地址左移后的地址字节 (含读写位)
 * @param[in] tx 发送缓冲区首地址 (DMA 期间须保持有效)
 * @param[in] len 发送字节数
 * @param[in] send_stop 是否发送 STOP (先写后读时写段不发 STOP)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT
 */
static int s_i2c_dma_write(I2C_TypeDef* i2c, uint8_t addr_byte, const uint8_t* tx, size_t len,
                           bool send_stop, uint32_t* budget)
{
    DMA_Channel_TypeDef* tx_ch = NULL;
    DMA_Channel_TypeDef* rx_ch = NULL;
    int ret = s_i2c_dma_channels((uintptr_t)i2c, &tx_ch, &rx_ch);
    if (ret != MINI_OK)
        return ret;
    if (len == 0U)
        return MINI_OK;

    ret = s_wait_flag(i2c, I2C_FLAG_BUSY, RESET, budget);
    if (ret != MINI_OK)
        return ret;
    I2C_GenerateSTART(i2c, ENABLE);
    ret = s_wait_flag(i2c, I2C_FLAG_SB, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    I2C_Send7bitAddress(i2c, addr_byte, I2C_Direction_Transmitter);
    ret = s_wait_flag(i2c, I2C_FLAG_ADDR, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    (void)s_wait_flag(i2c, I2C_FLAG_ADDR, RESET, budget); /* 读 SR2 清 ADDR */

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    s_i2c_dma_arm(tx_ch, (uint32_t)&i2c->DATAR, (uint32_t)tx, (uint32_t)len, DMA_DIR_PeripheralDST);
    I2C_DMACmd(i2c, ENABLE);

    ret = s_i2c_dma_wait_tc(tx_ch, budget);
    if (ret != MINI_OK)
        goto s_fail;
    ret = s_wait_flag(i2c, I2C_FLAG_BTF, SET, budget); /* 等末字节移出 */
    if (ret != MINI_OK)
        goto s_fail;
    I2C_DMACmd(i2c, DISABLE);
    s_i2c_dma_stop(tx_ch);

    if (I2C_GetFlagStatus(i2c, I2C_FLAG_AF) == SET)
    {
        /* 从机 NACK */
        I2C_ClearFlag(i2c, I2C_FLAG_AF);
        I2C_GenerateSTOP(i2c, ENABLE);
        return MINI_ERR_IO;
    }
    if (send_stop)
        I2C_GenerateSTOP(i2c, ENABLE);
    return MINI_OK;

s_fail:
    I2C_DMACmd(i2c, DISABLE);
    s_i2c_dma_stop(tx_ch);
    I2C_GenerateSTOP(i2c, ENABLE);
    return (ret == MINI_OK) ? MINI_ERR_IO : ret;
}

/**
 * @brief 主机 DMA 读段 (可重复 START + addr + DMA 搬运, 末字节经 DMAEOT/关 ACK 产生 NACK)
 * @param[in] i2c I2C 外设寄存器指针 (出参, 读数据寄存器)
 * @param[in] addr_byte 7-bit 地址左移后的地址字节 (含读写位)
 * @param[out] rx 接收缓冲区首地址 (DMA 期间须保持有效)
 * @param[in] len 接收字节数
 * @param[in] repeated_start 是否重复 START (先写后读时写段已发过地址)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, IO 错误返回 MINI_ERR_IO
 */
static int s_i2c_dma_read(I2C_TypeDef* i2c, uint8_t addr_byte, uint8_t* rx, size_t len,
                          bool repeated_start, uint32_t* budget)
{
    DMA_Channel_TypeDef* tx_ch = NULL;
    DMA_Channel_TypeDef* rx_ch = NULL;
    int ret = s_i2c_dma_channels((uintptr_t)i2c, &tx_ch, &rx_ch);
    if (ret != MINI_OK)
        return ret;
    if (len == 0U)
        return MINI_OK;
    if (!repeated_start)
    {
        ret = s_wait_flag(i2c, I2C_FLAG_BUSY, RESET, budget);
        if (ret != MINI_OK)
            return ret;
    }
    I2C_AcknowledgeConfig(i2c, ENABLE);
    if (len > 1U)
        I2C_DMALastTransferCmd(i2c, ENABLE); /* DMAEOT: 末字节自动 NACK */
    I2C_GenerateSTART(i2c, ENABLE);
    ret = s_wait_flag(i2c, I2C_FLAG_SB, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    I2C_Send7bitAddress(i2c, addr_byte, I2C_Direction_Receiver);
    ret = s_wait_flag(i2c, I2C_FLAG_ADDR, SET, budget);
    if (ret != MINI_OK)
        goto s_fail;
    if (len == 1U)
        I2C_AcknowledgeConfig(i2c, DISABLE); /* 单字节读: 清 ADDR 前先关 ACK */
    (void)s_wait_flag(i2c, I2C_FLAG_ADDR, RESET, budget); /* 读 SR2 清 ADDR */

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    s_i2c_dma_arm(rx_ch, (uint32_t)&i2c->DATAR, (uint32_t)rx, (uint32_t)len, DMA_DIR_PeripheralSRC);
    I2C_DMACmd(i2c, ENABLE);

    ret = s_i2c_dma_wait_tc(rx_ch, budget);
    I2C_DMACmd(i2c, DISABLE);
    I2C_DMALastTransferCmd(i2c, DISABLE);
    s_i2c_dma_stop(rx_ch);
    if (ret != MINI_OK)
        goto s_fail;
    I2C_GenerateSTOP(i2c, ENABLE);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    return MINI_OK;

s_fail:
    I2C_GenerateSTOP(i2c, ENABLE);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    return (ret == MINI_OK) ? MINI_ERR_IO : ret;
}

/**
 * @brief 按传输长度扩容超时预算 (每字节约 10 bit, 慢速时钟下耗时随长度线性增长)
 * @param[in] pdev I2C 设备指针 (仅读速率配置)
 * @param[in] bytes 总传输字节数 (写 + 读)
 * @param[in] timeout_ms 调用方给的超时毫秒数 (叠加扩容量)
 * @return 空转预算 (按 SystemCoreClock 折算)
 */
static uint32_t s_i2c_dma_budget(const struct hal_i2c_dev* pdev, size_t bytes, uint32_t timeout_ms)
{
    uint32_t speed = (pdev->cfg.clock_speed_hz != 0U) ? pdev->cfg.clock_speed_hz : 100000U;
    uint32_t xfer_ms = (((uint32_t)bytes * 10U * 1000U) / speed) + 1U;
    return ch32_poll_budget(timeout_ms + xfer_ms);
}

/* -------------------------------------------------------------------------- */

/* =============================================================================
 * 上层接口 (hal_i2c.h)
 * ============================================================================= */

/**
 * @brief 初始化 I2C 主机总线 (时钟 + SCL/SDA 引脚直投)
 * @param[in] host I2C 主机上下文指针 (出参, 写入总线句柄与状态)
 * @param[in] hw_idx 硬件索引 (原样保存)
 * @param[in] cfg I2C 总线配置指针 (拷贝至 host->cfg)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_i2c_bus_host_init(struct hal_i2c_bus_host* host, int hw_idx,
                          const struct hal_i2c_bus_config* cfg)
{
    if ((host == NULL) || (cfg == NULL))
        return MINI_ERR_INVAL;
    host->cfg = *cfg;
    host->i2c = cfg->i2c;
    host->hw_idx = hw_idx;
    host->ref_count = 0;
    host->bus_ready = false;
    host->hw_inited = false;
    host->active_cfg.clock_speed_hz = 0U;

    int ret = ch32_rcc_enable(cfg->i2c, cfg->i2c_clk_periph);
    if (ret != MINI_OK)
        return ret;

    /* SCL / SDA: 缺省复用开漏 */
    if (cfg->scl.port != 0U)
    {
        ret = ch32_gpio_port_clk(cfg->scl.port, cfg->scl.clk_bus);
        if (ret != MINI_OK)
            return ret;
        uint32_t scl_mode = (cfg->scl.mode != 0U) ? cfg->scl.mode : (uint32_t)GPIO_Mode_AF_OD;
        ret = ch32_gpio_cfg_pin(cfg->scl.port, ch32_pin_to_mask(cfg->scl.pin), scl_mode,
                                cfg->scl.speed, cfg->scl.pull);
        if (ret != MINI_OK)
            return ret;
    }
    if (cfg->sda.port != 0U)
    {
        ret = ch32_gpio_port_clk(cfg->sda.port, cfg->sda.clk_bus);
        if (ret != MINI_OK)
            return ret;
        uint32_t sda_mode = (cfg->sda.mode != 0U) ? cfg->sda.mode : (uint32_t)GPIO_Mode_AF_OD;
        ret = ch32_gpio_cfg_pin(cfg->sda.port, ch32_pin_to_mask(cfg->sda.pin), sda_mode,
                                cfg->sda.speed, cfg->sda.pull);
        if (ret != MINI_OK)
            return ret;
    }

    host->hw_inited = true;
    return MINI_OK;
}

/**
 * @brief 去初始化 I2C 主机总线 (关外设, 不开总线时钟)
 * @param[in] host I2C 主机上下文指针 (出参, 写回状态)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_i2c_bus_host_deinit(struct hal_i2c_bus_host* host)
{
    if (host == NULL)
        return MINI_ERR_INVAL;
    if (host->hw_inited)
    {
        I2C_TypeDef* i2c = (I2C_TypeDef*)host->i2c;
        I2C_Cmd(i2c, DISABLE);
        host->hw_inited = false;
        host->bus_ready = false;
    }
    return MINI_OK;
}

/**
 * @brief 初始化 I2C 设备 (绑定总线与设备配置)
 * @param[in] pdev I2C 设备指针 (出参)
 * @param[in] host I2C 主机上下文 (总线必须已初始化)
 * @param[in] dev_cfg I2C 设备配置指针 (拷贝至 pdev->cfg)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_init(struct hal_i2c_dev* pdev, struct hal_i2c_bus_host* host,
                     const struct hal_i2c_device_config* dev_cfg)
{
    if ((pdev == NULL) || (host == NULL) || (dev_cfg == NULL))
        return MINI_ERR_INVAL;
    pdev->ctlr = host;
    pdev->cfg = *dev_cfg;
    pdev->hw_open = 0;
    return MINI_OK;
}

/**
 * @brief 去初始化 I2C 设备 (关闭总线引用)
 * @param[in] pdev I2C 设备指针 (已绑定总线)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_deinit(struct hal_i2c_dev* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    if (pdev->hw_open > 0)
    {
        int ret = hal_i2c_dev_hw_close(pdev);
        if (ret != MINI_OK)
            return ret;
    }
    pdev->ctlr = NULL;
    return MINI_OK;
}

/**
 * @brief 打开 I2C 设备 (应用配置并累加总线引用计数)
 * @param[in] pdev I2C 设备指针 (已绑定总线和配置)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL, 配置失败透传错误码
 */
int hal_i2c_dev_hw_open(struct hal_i2c_dev* pdev)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    int ret = s_i2c_apply_dev_cfg(pdev->ctlr, &pdev->cfg);
    if (ret != MINI_OK)
        return ret;
    pdev->ctlr->ref_count++;
    pdev->hw_open++;
    return MINI_OK;
}

/**
 * @brief 关闭 I2C 设备 (累减总线引用计数, 归零后发 STOP)
 * @param[in] pdev I2C 设备指针 (已绑定总线)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_hw_close(struct hal_i2c_dev* pdev)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    struct hal_i2c_bus_host* host = pdev->ctlr;
    if (pdev->hw_open > 0)
    {
        pdev->hw_open--;
        if (host->ref_count > 0)
            host->ref_count--;
    }
    if (host->ref_count == 0)
    {
        I2C_TypeDef* i2c = (I2C_TypeDef*)host->i2c;
        I2C_GenerateSTOP(i2c, ENABLE);
        host->bus_ready = false;
    }
    return MINI_OK;
}

/**
 * @brief I2C 主机同步写 (内部左移 7-bit 地址, 支持重复传输)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[in] tx 发送缓冲区 (允许空指针配合 len=0)
 * @param[in] len 发送字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT,
 *         参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((tx == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = ch32_poll_budget(timeout_ms);
    return s_master_write(i2c, addr_byte, tx, len, true, &budget);
}

/**
 * @brief I2C 主机同步读 (内部左移 7-bit 地址, 支持重复传输)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[out] rx 接收缓冲区 (允许空指针配合 len=0)
 * @param[in] len 接收字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, IO 错误返回 MINI_ERR_IO,
 *         参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((rx == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = ch32_poll_budget(timeout_ms);
    return s_master_read(i2c, addr_byte, rx, len, false, &budget);
}

/**
 * @brief I2C 主机同步先写后读 (Repeated START, 写段不发 STOP)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[in] tx 发送缓冲区 (NULL = 纯读)
 * @param[out] rx 接收缓冲区 (NULL = 纯写)
 * @param[in] len 读写字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT,
 *         参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_sync(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                 uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    if ((tx == NULL) && (rx == NULL))
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = ch32_poll_budget(timeout_ms);

    if ((tx != NULL) && (rx == NULL))
        return s_master_write(i2c, addr_byte, tx, len, true, &budget);
    if ((tx == NULL) && (rx != NULL))
        return s_master_read(i2c, addr_byte, rx, len, false, &budget);

    /* 先写后读 (Repeated START, 写段不发 STOP) */
    int ret = s_master_write(i2c, addr_byte, tx, len, false, &budget);
    if (ret != MINI_OK)
        return ret;
    return s_master_read(i2c, addr_byte, rx, len, true, &budget);
}

/**
 * @brief I2C 主机 DMA 写 (内部左移 7-bit 地址, 预算按长度/速率扩容)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[in] tx 发送缓冲区 (允许空指针配合 len=0, DMA 期间须保持有效)
 * @param[in] len 发送字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (叠加传输耗时扩容量)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT,
 *         无 DMA 映射返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_dma_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((tx == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = s_i2c_dma_budget(pdev, len, timeout_ms);
    return s_i2c_dma_write(i2c, addr_byte, tx, len, true, &budget);
}

/**
 * @brief I2C 主机 DMA 读 (内部左移 7-bit 地址, 预算按长度/速率扩容)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[out] rx 接收缓冲区 (允许空指针配合 len=0, DMA 期间须保持有效)
 * @param[in] len 接收字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (叠加传输耗时扩容量)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, IO 错误返回 MINI_ERR_IO,
 *         无 DMA 映射返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_dma_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if ((rx == NULL) && (len != 0U))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = s_i2c_dma_budget(pdev, len, timeout_ms);
    return s_i2c_dma_read(i2c, addr_byte, rx, len, false, &budget);
}

/**
 * @brief I2C 主机 DMA 先写后读 (Repeated START, 写段不发 STOP)
 * @param[in] pdev I2C 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[in] tx 发送缓冲区 (NULL = 纯读)
 * @param[out] rx 接收缓冲区 (NULL = 纯写)
 * @param[in] len 读写字节数 (不超过 HAL_I2C_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (叠加传输耗时扩容量)
 * @return 成功返回 MINI_OK, 从机 NACK 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT,
 *         无 DMA 映射返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_i2c_dma_write_then_read(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx,
                                size_t len, uint32_t timeout_ms)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if (len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;
    if ((tx == NULL) && (rx == NULL))
        return MINI_ERR_INVAL;
    I2C_TypeDef* i2c = (I2C_TypeDef*)pdev->ctlr->i2c;
    uint8_t addr_byte = (uint8_t)((pdev->cfg.address & 0x7FU) << 1);
    uint32_t budget = s_i2c_dma_budget(pdev, 2U * len, timeout_ms);

    if ((tx != NULL) && (rx == NULL))
        return s_i2c_dma_write(i2c, addr_byte, tx, len, true, &budget);
    if ((tx == NULL) && (rx != NULL))
        return s_i2c_dma_read(i2c, addr_byte, rx, len, false, &budget);

    /* 先写后读 (Repeated START, 写段不发 STOP) */
    int ret = s_i2c_dma_write(i2c, addr_byte, tx, len, false, &budget);
    if (ret != MINI_OK)
        return ret;
    return s_i2c_dma_read(i2c, addr_byte, rx, len, true, &budget);
}
