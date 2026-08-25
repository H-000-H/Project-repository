/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_spi_ch32v30x.c
 *@brief CH32V307 SPI HAL 强符号实现 (覆盖 mini_tree/hal/spi/hal_spi.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头。
 *   直投约定 (对齐 hal_spi.h WCH 注释):
 *   - spi = SPIx_BASE, spi_clk_periph = RCC_APBxPeriph_SPIx;
 *   - device mode 编码 bit1=CPOL / bit0=CPHA;
 *   - nss / bit_order / transfer_direction / data_width 直投 WCH 宏 (0=缺省:
 *     软 NSS / MSB / 全双工 / 8bit);
 *   - clock_speed_hz 按外设所在 APB 总线实际频率折算最小分频 (>=2);
 *   - 主机同步传输走 CPU polling, CS 为软件 GPIO (cs_port/cs_pin);
 *   - DMA 路径: DMA1 固定映射 (SPI1 RX/TX = CH2/CH3, SPI2 RX/TX = CH4/CH5),
 *     SPI3 请求位于 DMA2 未佐证 → AUTO 降级 polling / 强制 DMA 返回 NOTSUPP;
 *   - 异步 / 从机路径本板未接线, 返回 MINI_ERR_NOTSUPP。
 */

#include "ch32v30x_hal_common.h"
#include "hal_spi.h"

/* =============================================================================
 * 内部辅助 (分频选择 / 引脚 / 软件 CS / 配置应用)
 * ============================================================================= */

/**
 * @brief 按目标频率选取最小分频 (SPI_BaudRatePrescaler_2..256, 步进 0x0008)
 * @param[in] spi_base SPIx_BASE (用于折算所在 APB 总线频率)
 * @param[in] clock_speed_hz 目标时钟 (0 = 最慢 256 分频)
 * @return SPI_BaudRatePrescaler_* 宏值
 */
static uint16_t s_spi_best_prescaler(uintptr_t spi_base, uint32_t clock_speed_hz)
{
    if (clock_speed_hz == 0U)
        return (uint16_t)0x0038U; /* _256 */
    uint32_t src = ch32_apb_freq(spi_base);
    uint32_t div = 2U;
    uint16_t val = 0x0000U;
    while ((div < 256U) && ((src / div) > clock_speed_hz))
    {
        div <<= 1;
        val = (uint16_t)(val + 0x0008U);
    }
    return val;
}

/**
 * @brief 配置单个 SPI 引脚 (mode 缺省时按角色兜底)
 * @param[in] pin 引脚配置 (port=0 表示未接线, 直接跳过)
 * @param[in] fallback_mode mode/af 均缺省时的兜底 GPIOMode
 * @return 成功返回 MINI_OK
 */
static int s_spi_pin_setup(const struct hal_spi_pin_cfg* pin, uint32_t fallback_mode)
{
    if (pin->port == 0U)
        return MINI_OK;
    int ret = ch32_gpio_port_clk(pin->port, pin->clk_bus);
    if (ret != MINI_OK)
        return ret;
    uint32_t mode = (pin->mode != 0U) ? pin->mode : ((pin->af != 0U) ? pin->af : fallback_mode);
    return ch32_gpio_cfg_pin(pin->port, ch32_pin_to_mask(pin->pin), mode, pin->speed, pin->pull);
}

/**
 * @brief 软件 CS 初始化 (推挽输出 + 默认拉高, cs_pin<0 表示无 CS)
 * @param[in] cfg SPI 设备配置 (cs_port/cs_pin/cs_clk_periph)
 * @return 成功返回 MINI_OK, 端口时钟失败透传错误码
 */
static int s_spi_cs_setup(const struct hal_spi_device_config* cfg)
{
    if ((cfg->cs_pin < 0) || (cfg->cs_port == 0U))
        return MINI_OK;
    int ret = ch32_gpio_port_clk(cfg->cs_port, cfg->cs_clk_periph);
    if (ret != MINI_OK)
        return ret;
    ret = ch32_gpio_cfg_pin(cfg->cs_port, ch32_pin_to_mask((uint32_t)cfg->cs_pin),
                            (uint32_t)GPIO_Mode_Out_PP, 0U, 0U);
    if (ret != MINI_OK)
        return ret;
    ((GPIO_TypeDef*)cfg->cs_port)->BSHR = ch32_pin_to_mask((uint32_t)cfg->cs_pin);
    return MINI_OK;
}

/**
 * @brief 软件 CS 电平控制 (0=拉低选中, 非 0=拉高释放)
 * @param[in] cfg SPI 设备配置
 * @param[in] level 目标电平 (0=低, 非 0=高)
 */
static void s_spi_cs_set(const struct hal_spi_device_config* cfg, int level)
{
    if ((cfg->cs_pin < 0) || (cfg->cs_port == 0U))
        return;
    GPIO_TypeDef* port = (GPIO_TypeDef*)cfg->cs_port;
    uint32_t mask = ch32_pin_to_mask((uint32_t)cfg->cs_pin);
    if (level != 0)
        port->BSHR = mask;
    else
        port->BCR = mask;
}

/**
 * @brief 应用 device 配置到 SPI 外设 (每次传输前比对, 多设备共享总线时随路切换)
 * @param[in] host SPI 主机上下文
 * @param[in] cfg 目标设备配置 (与 active_cfg 一致且已就绪则跳过)
 * @return 成功返回 MINI_OK, 硬件配置失败返回错误码
 */
static int s_spi_apply_dev_cfg(struct hal_spi_bus_host* host,
                               const struct hal_spi_device_config* cfg)
{
    if (host->bus_ready && (host->active_cfg.mode == cfg->mode) &&
        (host->active_cfg.clock_speed_hz == cfg->clock_speed_hz) &&
        (host->active_cfg.data_width == cfg->data_width) &&
        (host->active_cfg.bit_order == cfg->bit_order) && (host->active_cfg.nss == cfg->nss) &&
        (host->active_cfg.transfer_direction == cfg->transfer_direction))
    {
        return MINI_OK;
    }

    SPI_TypeDef* spi = (SPI_TypeDef*)host->spi;
    SPI_Cmd(spi, DISABLE);

    SPI_InitTypeDef si;
    si.SPI_Direction = (cfg->transfer_direction != 0U) ? (uint16_t)cfg->transfer_direction :
                                                         SPI_Direction_2Lines_FullDuplex;
    si.SPI_Mode = (host->cfg.bus_role == HAL_SPI_BUS_ROLE_SLAVE) ? SPI_Mode_Slave : SPI_Mode_Master;
    si.SPI_DataSize = (cfg->data_width != 0U) ? (uint16_t)cfg->data_width : SPI_DataSize_8b;
    si.SPI_CPOL = ((cfg->mode & 0x2) != 0) ? SPI_CPOL_High : SPI_CPOL_Low;
    si.SPI_CPHA = ((cfg->mode & 0x1) != 0) ? SPI_CPHA_2Edge : SPI_CPHA_1Edge;
    si.SPI_NSS = (cfg->nss == 0U) ? SPI_NSS_Soft : (uint16_t)cfg->nss;
    si.SPI_BaudRatePrescaler = s_spi_best_prescaler(host->spi, cfg->clock_speed_hz);
    si.SPI_FirstBit = (cfg->bit_order != 0U) ? (uint16_t)cfg->bit_order : (uint16_t)0x0000U;
    si.SPI_CRCPolynomial = (cfg->crc_poly != 0U) ? (uint16_t)cfg->crc_poly : 7U;
    SPI_Init(spi, &si);
    SPI_Cmd(spi, ENABLE);

    host->active_cfg = *cfg;
    host->bus_ready = true;
    return MINI_OK;
}

/* =============================================================================
 * DMA 全双工传输 (DMA1 固定映射: SPI1 RX/TX=CH2/CH3, SPI2 RX/TX=CH4/CH5)
 * ============================================================================= */

/* 哑元缓冲: tx 缺省时的发送源 / rx 缺省时的接收丢弃槽 */
static uint16_t s_spi_dma_dummy = 0xFFFFU;

/**
 * @brief SPI 基址 → DMA1 收发通道 (固定映射查表)
 * @param[in] spi_base SPIx_BASE
 * @param[out] rx_ch 接收通道输出指针 (外设→内存)
 * @param[out] tx_ch 发送通道输出指针 (内存→外设)
 * @return 成功返回 MINI_OK; SPI3 请求位于 DMA2 映射未佐证, 返回 MINI_ERR_NOTSUPP
 */
static int s_spi_dma_channels(uintptr_t spi_base, DMA_Channel_TypeDef** rx_ch,
                              DMA_Channel_TypeDef** tx_ch)
{
    switch (spi_base)
    {
    case SPI1_BASE:
        *rx_ch = DMA1_Channel2;
        *tx_ch = DMA1_Channel3;
        return MINI_OK;
    case SPI2_BASE:
        *rx_ch = DMA1_Channel4;
        *tx_ch = DMA1_Channel5;
        return MINI_OK;
    default:
        *rx_ch = NULL;
        *tx_ch = NULL;
        return MINI_ERR_NOTSUPP;
    }
}

/**
 * @brief 停止 DMA 通道 (去使能 + 清全部标志)
 * @param[in] ch DMA 通道指针 (NULL 安全)
 */
static void s_spi_dma_stop(DMA_Channel_TypeDef* ch)
{
    if (ch == NULL)
        return;
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    DMA_Cmd(ch, DISABLE);
    DMA_ClearFlag(0xFU << ((idx - 1U) * 4U)); /* GL/TC/HT/TE 全清 */
}

/**
 * @brief 配置并启动一个 DMA 通道 (不等待, 由调用方统一等完成)
 * @param[in] ch DMA 通道指针 (出参, 写配置寄存器)
 * @param[in] periph_addr 外设数据寄存器地址 (&SPIx->DATAR)
 * @param[in] mem_addr 内存缓冲区地址 (哑元缓冲时不自增)
 * @param[in] units 传输单元数 (1..65535)
 * @param[in] dir DMA_DIR_PeripheralDST / DMA_DIR_PeripheralSRC (直投)
 * @param[in] psize 外设数据宽度 (8b=Byte, 16b=HalfWord)
 * @param[in] msize 内存数据宽度 (同 psize)
 * @param[in] mem_inc 内存地址自增 (false=定址读/写哑元单点)
 */
static void s_spi_dma_arm(DMA_Channel_TypeDef* ch, uint32_t periph_addr, uint32_t mem_addr,
                          uint32_t units, uint32_t dir, uint32_t psize, uint32_t msize,
                          bool mem_inc)
{
    uint32_t idx = ((uint32_t)((uintptr_t)ch - DMA1_Channel1_BASE) / 0x14U) + 1U;

    DMA_Cmd(ch, DISABLE);

    DMA_InitTypeDef di;
    di.DMA_PeripheralBaseAddr = periph_addr;
    di.DMA_MemoryBaseAddr = mem_addr;
    di.DMA_DIR = dir;
    di.DMA_BufferSize = units;
    di.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    di.DMA_MemoryInc = mem_inc ? DMA_MemoryInc_Enable : DMA_MemoryInc_Disable;
    di.DMA_PeripheralDataSize = psize;
    di.DMA_MemoryDataSize = msize;
    di.DMA_Mode = DMA_Mode_Normal;
    di.DMA_Priority = DMA_Priority_High;
    di.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(ch, &di);

    DMA_ClearFlag(1U << (((idx - 1U) * 4U) + 1U)); /* TC */
    DMA_ClearFlag(1U << (((idx - 1U) * 4U) + 3U)); /* TE */
    DMA_Cmd(ch, ENABLE);
}

/**
 * @brief DMA 全双工传输 (RX+TX 双通道, 等 RX 完成与 BSY 清除后关双通道)
 * @param[in] spi SPI 外设寄存器指针 (出参, 写 DMA 请求位)
 * @param[in] rx_ch / tx_ch DMA 收发通道 (s_spi_dma_channels 返回值, 非空)
 * @param[in] tx 发送缓冲区 (NULL = 发送哑元 0xFFFF 纯读)
 * @param[out] rx 接收缓冲区 (NULL = 丢弃到哑元槽)
 * @param[in] units 传输单元数 (8b=字节数, 16b=半字数)
 * @param[in] is_16bit 16bit 数据宽度 (决定外设/内存宽度宏)
 * @param[in,out] budget 空转预算指针 (消耗至 0 返回超时)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, 传输错误返回 MINI_ERR_IO
 */
static int s_spi_dma_xfer(SPI_TypeDef* spi, DMA_Channel_TypeDef* rx_ch, DMA_Channel_TypeDef* tx_ch,
                          const uint8_t* tx, uint8_t* rx, uint32_t units, bool is_16bit,
                          uint32_t* budget)
{
    if ((units == 0U) || (units > 0xFFFFU))
        return MINI_ERR_INVAL;
    const uint32_t psize = is_16bit ? DMA_PeripheralDataSize_HalfWord : DMA_PeripheralDataSize_Byte;
    const uint32_t msize = is_16bit ? DMA_MemoryDataSize_HalfWord : DMA_MemoryDataSize_Byte;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    s_spi_dma_arm(rx_ch, (uint32_t)&spi->DATAR,
                  (rx != NULL) ? (uint32_t)rx : (uint32_t)&s_spi_dma_dummy, units,
                  DMA_DIR_PeripheralSRC, psize, msize, rx != NULL);
    s_spi_dma_arm(tx_ch, (uint32_t)&spi->DATAR,
                  (tx != NULL) ? (uint32_t)tx : (uint32_t)&s_spi_dma_dummy, units,
                  DMA_DIR_PeripheralDST, psize, msize, tx != NULL);
    SPI_I2S_DMACmd(spi, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, ENABLE);

    /* 等 RX 通道 TC (全双工下最后一帧收完即整体完成) */
    uint32_t idx = ((uint32_t)((uintptr_t)rx_ch - DMA1_Channel1_BASE) / 0x14U) + 1U;
    const uint32_t tc = 1U << (((idx - 1U) * 4U) + 1U);
    const uint32_t te = 1U << (((idx - 1U) * 4U) + 3U);
    int ret = MINI_OK;
    while (DMA_GetFlagStatus(tc) == RESET)
    {
        if (DMA_GetFlagStatus(te) != RESET)
        {
            ret = MINI_ERR_IO;
            break;
        }
        if (*budget == 0U)
        {
            ret = MINI_ERR_TIMEOUT;
            break;
        }
        (*budget)--;
    }
    SPI_I2S_DMACmd(spi, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, DISABLE);
    s_spi_dma_stop(rx_ch);
    s_spi_dma_stop(tx_ch);
    if (ret != MINI_OK)
        return ret;

    while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_BSY) != RESET)
    {
        if (*budget == 0U)
            return MINI_ERR_TIMEOUT;
        (*budget)--;
    }
    return MINI_OK;
}

/* -------------------------------------------------------------------------- */

/* =============================================================================
 * 上层接口 (hal_spi.h)
 * ============================================================================= */

/**
 * @brief 初始化 SPI 主机总线 (时钟 + MOSI/MISO/SCLK 引脚直投)
 * @param[in] host SPI 主机上下文指针 (出参, 写入总线句柄与状态)
 * @param[in] hw_idx 硬件索引 (原样保存)
 * @param[in] cfg SPI 总线配置指针 (拷贝至 host->cfg)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_spi_bus_host_init(struct hal_spi_bus_host* host, int hw_idx,
                          const struct hal_spi_bus_config* cfg)
{
    if ((host == NULL) || (cfg == NULL))
        return MINI_ERR_INVAL;
    host->cfg = *cfg;
    host->spi = cfg->spi;
    host->hw_idx = hw_idx;
    host->ref_count = 0;
    host->bus_ready = false;
    host->hw_inited = false;
    host->active_cfg.mode = -1; /* 强制首次传输应用配置 */

    int ret = ch32_rcc_enable(cfg->spi, cfg->spi_clk_periph);
    if (ret != MINI_OK)
        return ret;

    if (cfg->bus_role == HAL_SPI_BUS_ROLE_SLAVE)
    {
        /* 从机: MOSI/MISO/SCLK 均为复用输入/输出, 缺省按主机方向兜底 */
        ret = s_spi_pin_setup(&cfg->mosi, (uint32_t)GPIO_Mode_IN_FLOATING);
    }
    else
    {
        ret = s_spi_pin_setup(&cfg->mosi, (uint32_t)GPIO_Mode_AF_PP);
    }
    if (ret != MINI_OK)
        return ret;
    ret = s_spi_pin_setup(&cfg->miso, (uint32_t)GPIO_Mode_IN_FLOATING);
    if (ret != MINI_OK)
        return ret;
    ret = s_spi_pin_setup(&cfg->sclk, (uint32_t)GPIO_Mode_AF_PP);
    if (ret != MINI_OK)
        return ret;

    host->hw_inited = true;
    return MINI_OK;
}

/**
 * @brief 去初始化 SPI 主机总线 (关外设, 不开总线时钟)
 * @param[in] host SPI 主机上下文指针 (出参, 写回状态)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_spi_bus_host_deinit(struct hal_spi_bus_host* host)
{
    if (host == NULL)
        return MINI_ERR_INVAL;
    if (host->hw_inited)
    {
        SPI_TypeDef* spi = (SPI_TypeDef*)host->spi;
        SPI_Cmd(spi, DISABLE);
        host->hw_inited = false;
        host->bus_ready = false;
    }
    return MINI_OK;
}

/**
 * @brief 初始化 SPI 设备 (绑定总线 + 软件 CS 引脚初始化)
 * @param[in] pdev SPI 设备指针 (出参)
 * @param[in] host SPI 主机上下文 (总线必须已初始化)
 * @param[in] dev_cfg SPI 设备配置指针 (拷贝至 pdev->cfg)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL, CS 失败透传错误码
 */
int hal_spi_dev_init(struct hal_spi_dev* pdev, struct hal_spi_bus_host* host,
                     const struct hal_spi_device_config* dev_cfg)
{
    if ((pdev == NULL) || (host == NULL) || (dev_cfg == NULL))
        return MINI_ERR_INVAL;
    pdev->ctlr = host;
    pdev->cfg = *dev_cfg;
    pdev->hw_open = 0;
    return s_spi_cs_setup(dev_cfg);
}

/**
 * @brief 打开 SPI 设备 (应用配置并累加总线引用计数)
 * @param[in] pdev SPI 设备指针 (已绑定总线和配置)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_spi_dev_hw_open(struct hal_spi_dev* pdev)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    struct hal_spi_bus_host* host = pdev->ctlr;
    int ret = s_spi_apply_dev_cfg(host, &pdev->cfg);
    if (ret != MINI_OK)
        return ret;
    host->ref_count++;
    pdev->hw_open = 1;
    return MINI_OK;
}

/**
 * @brief 关闭 SPI 设备 (累减总线引用计数, 归零后关外设)
 * @param[in] pdev SPI 设备指针 (已绑定总线)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_spi_dev_hw_close(struct hal_spi_dev* pdev)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    struct hal_spi_bus_host* host = pdev->ctlr;
    if (pdev->hw_open != 0)
    {
        pdev->hw_open = 0;
        if (host->ref_count > 0)
            host->ref_count--;
    }
    if (host->ref_count == 0)
    {
        SPI_TypeDef* spi = (SPI_TypeDef*)host->spi;
        SPI_Cmd(spi, DISABLE);
        host->bus_ready = false;
    }
    return MINI_OK;
}

/**
 * @brief 主机同步全双工传输 (CPU polling, 支持 8/16bit, 软件 CS 包裹)
 * @param[in] pdev SPI 设备指针 (必须已绑定总线并完成 dev_init)
 * @param[in] tx 发送缓冲区 (NULL = 发送 0xFF 纯读)
 * @param[out] rx 接收缓冲区 (NULL = 纯写丢弃接收)
 * @param[in] len 传输字节数 (不超过 HAL_SPI_MAX_XFER)
 * @param[in] timeout_ms 超时毫秒数 (按 SystemCoreClock 折算空转预算)
 * @param[in] xfer_mode 传输模式 (DMA 走 DMA1 固定映射; 无映射时 AUTO 降级 polling,
 *                      强制 DMA 返回 MINI_ERR_NOTSUPP)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT,
 *         DMA 未支持返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_spi_sync(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                 uint32_t timeout_ms, uint32_t xfer_mode)
{
    if ((pdev == NULL) || (pdev->ctlr == NULL))
        return MINI_ERR_INVAL;
    if (len > HAL_SPI_MAX_XFER)
        return MINI_ERR_INVAL;

    struct hal_spi_bus_host* host = pdev->ctlr;
    int ret = s_spi_apply_dev_cfg(host, &pdev->cfg);
    if (ret != MINI_OK)
        return ret;
    SPI_TypeDef* spi = (SPI_TypeDef*)host->spi;
    uint32_t budget = ch32_poll_budget(timeout_ms);
    const int is_16bit = (pdev->cfg.data_width == SPI_DataSize_16b);

    /* DMA 路径 (固定映射; 无映射外设: 强制 DMA 拒绝, AUTO 降级 polling) */
    if ((xfer_mode == HAL_SPI_XFER_DMA) ||
        ((xfer_mode == HAL_SPI_XFER_AUTO) && (pdev->ctlr->cfg.dma_tx.dma_enable != 0U)))
    {
        DMA_Channel_TypeDef* rx_ch = NULL;
        DMA_Channel_TypeDef* tx_ch = NULL;
        ret = s_spi_dma_channels(host->spi, &rx_ch, &tx_ch);
        if (ret == MINI_OK)
        {
            uint32_t units = is_16bit ? (uint32_t)(len / 2U) : (uint32_t)len;
            s_spi_cs_set(&pdev->cfg, 0);
            ret = s_spi_dma_xfer(spi, rx_ch, tx_ch, tx, rx, units, is_16bit != 0, &budget);
            s_spi_cs_set(&pdev->cfg, 1);
            return ret;
        }
        if (xfer_mode == HAL_SPI_XFER_DMA)
            return MINI_ERR_NOTSUPP;
        /* AUTO 且无映射: 降级下方 polling */
    }

    s_spi_cs_set(&pdev->cfg, 0);

    if (is_16bit)
    {
        size_t half = len / 2U;
        for (size_t i = 0; i < half; i++)
        {
            uint16_t out = 0xFFFFU;
            if (tx != NULL)
                out = (uint16_t)((uint16_t)tx[2U * i] | ((uint16_t)tx[2U * i + 1U] << 8));
            while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_TXE) == RESET)
            {
                if (budget == 0U)
                    goto s_timeout;
                budget--;
            }
            SPI_I2S_SendData(spi, out);
            while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_RXNE) == RESET)
            {
                if (budget == 0U)
                    goto s_timeout;
                budget--;
            }
            uint16_t in = (uint16_t)SPI_I2S_ReceiveData(spi);
            if (rx != NULL)
            {
                rx[2U * i] = (uint8_t)(in & 0xFFU);
                rx[2U * i + 1U] = (uint8_t)(in >> 8);
            }
        }
    }
    else
    {
        for (size_t i = 0; i < len; i++)
        {
            uint8_t out = (tx != NULL) ? tx[i] : 0xFFU;
            while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_TXE) == RESET)
            {
                if (budget == 0U)
                    goto s_timeout;
                budget--;
            }
            SPI_I2S_SendData(spi, out);
            while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_RXNE) == RESET)
            {
                if (budget == 0U)
                    goto s_timeout;
                budget--;
            }
            uint8_t in = (uint8_t)SPI_I2S_ReceiveData(spi);
            if (rx != NULL)
                rx[i] = in;
        }
    }

    while (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_BSY) != RESET)
    {
        if (budget == 0U)
            goto s_timeout;
        budget--;
    }
    s_spi_cs_set(&pdev->cfg, 1);
    return MINI_OK;

s_timeout:
    s_spi_cs_set(&pdev->cfg, 1);
    return MINI_ERR_TIMEOUT;
}

/**
 * @brief 异步传输 (WCH 路径不支持, 对齐 hal_spi.h 注释; 同步请用 hal_spi_sync)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_spi_transfer_async(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                           hal_spi_callback_t cb, void* userdata)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(tx);
    COMPAT_UNUSED_PARAM(rx);
    COMPAT_UNUSED_PARAM(len);
    COMPAT_UNUSED_PARAM(cb);
    COMPAT_UNUSED_PARAM(userdata);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 轮询异步传输完成状态 (与 transfer_async 对称, 本板未实现)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_spi_transfer_poll(struct hal_spi_dev* pdev, uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 获取异步传输接收结果 (与 transfer_async 对称, 本板未实现)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_spi_get_trans_result(struct hal_spi_dev* pdev, uint8_t* rx_data, size_t rx_cap,
                             size_t* trans_len, uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(rx_data);
    COMPAT_UNUSED_PARAM(rx_cap);
    COMPAT_UNUSED_PARAM(trans_len);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 从机同步传输 (本板未接从机, 返回 MINI_ERR_NOTSUPP)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_spi_slave_sync(struct hal_spi_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len,
                       uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(tx);
    COMPAT_UNUSED_PARAM(rx);
    COMPAT_UNUSED_PARAM(len);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 从机发送队列排队 (本板未接从机, 返回 MINI_ERR_NOTSUPP)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_spi_slave_queue_tx(struct hal_spi_dev* pdev, const uint8_t* data, size_t len,
                           uint32_t timeout_ms)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(data);
    COMPAT_UNUSED_PARAM(len);
    COMPAT_UNUSED_PARAM(timeout_ms);
    return MINI_ERR_NOTSUPP;
}
