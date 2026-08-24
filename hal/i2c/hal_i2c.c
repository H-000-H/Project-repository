/* SPDX-License-Identifier: Apache-2.0 */
/*
 * I2C HAL — STM32F4 实现
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 LL 库。
 * - 主机写/读/写后读: START → 地址 → 数据 → STOP
 * - DMA 缓冲由调用方动态传入
 */
#include "hal_i2c.h"
#include "compiler_compat.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_i2c.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "dt_config_gen.h"

#ifndef DTC_GEN_STM32_I2C_HOST_MAX
#define DTC_GEN_STM32_I2C_HOST_MAX  3
#endif
#ifndef DTC_GEN_STM32_I2C_MAX_XFER
#define DTC_GEN_STM32_I2C_MAX_XFER  512U
#endif

#undef HAL_I2C_HOST_MAX
#define HAL_I2C_HOST_MAX   DTC_GEN_STM32_I2C_HOST_MAX
#undef HAL_I2C_MAX_XFER
#define HAL_I2C_MAX_XFER   DTC_GEN_STM32_I2C_MAX_XFER

static void hal_i2c_dma_init(const struct hal_i2c_dma_config* cfg, uint32_t direction);

/**
 * @brief 配置 I2C SCL/SDA 引脚为开漏复用
 * @param gpio_cfg 引脚配置指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
static int hal_i2c_gpio_config(const struct hal_i2c_pin_cfg* gpio_cfg)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (!gpio_cfg || !gpio_cfg->port || !gpio_cfg->pin || !gpio_cfg->clk_bus)
        return MINI_ERR_INVAL;

    GPIO_InitStruct.Pin        = gpio_cfg->pin;
    GPIO_InitStruct.Mode       = gpio_cfg->mode ? gpio_cfg->mode : LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed      = gpio_cfg->speed ? gpio_cfg->speed : LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = gpio_cfg->output_type ? gpio_cfg->output_type
                                                       : LL_GPIO_OUTPUT_OPENDRAIN;
    GPIO_InitStruct.Pull       = gpio_cfg->pull ? gpio_cfg->pull : LL_GPIO_PULL_UP;
    GPIO_InitStruct.Alternate  = gpio_cfg->af;

    LL_AHB1_GRP1_EnableClock(gpio_cfg->clk_bus);
    LL_GPIO_Init((GPIO_TypeDef*)gpio_cfg->port, &GPIO_InitStruct);
    return MINI_OK;
}

/**
 * @brief 将 I2C 引脚复位为模拟输入
 * @param gpio_cfg 引脚配置指针, 为 NULL 或 port/pin 为空时跳过
 */
static void hal_i2c_gpio_reset(const struct hal_i2c_pin_cfg* gpio_cfg)
{
    GPIO_TypeDef* port;

    if (!gpio_cfg || !gpio_cfg->port || !gpio_cfg->pin)
        return;

    port = (GPIO_TypeDef*)gpio_cfg->port;
    LL_GPIO_SetPinMode(port, gpio_cfg->pin, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinPull(port, gpio_cfg->pin, LL_GPIO_PULL_NO);
}

/**
 * @brief 轮询等待 I2C 标志位置位，遇 AF 或超时返回错误
 * @param i2c I2C 外设指针
 * @param is_set 标志检测函数指针
 * @param timeout_ms 超时 (ms)
 * @param start 起始 tick (HAL_GetTick)
 * @return 成功返回 MINI_OK, AF 返回 MINI_ERR_IO, 超时返回 MINI_ERR_TIMEOUT
 */
static int wait_flag_set(I2C_TypeDef* i2c, uint32_t (*is_set)(I2C_TypeDef*), uint32_t timeout_ms, uint32_t start)
{
    while (!is_set(i2c))
    {
        if (LL_I2C_IsActiveFlag_AF(i2c))
        {
            LL_I2C_ClearFlag_AF(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_IO;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }
    return MINI_OK;
}

/**
 * @brief 轮询等待 I2C 总线空闲
 * @param i2c I2C 外设指针
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int wait_not_busy(I2C_TypeDef* i2c, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (LL_I2C_IsActiveFlag_BUSY(i2c))
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }
    return MINI_OK;
}

/**
 * @brief 构造 7-bit I2C 地址字节 ((addr<<1)|rw)
 * @param cfg 设备配置指针 (含 7-bit address)
 * @param read 非 0 表示读方向
 * @return 地址字节
 * @note 10-bit 寻址在 hw_open 已拒 (NOTSUPP), 此处仅 7-bit 路径
 */
static uint8_t i2c_addr_byte(const struct hal_i2c_device_config* cfg, int read)
{
    return (uint8_t)(((cfg->address & 0x7FU) << 1) | (read ? 1U : 0U));
}

/**
 * @brief 发送 START 条件并写入 7-bit 从机地址
 * @param i2c I2C 外设指针
 * @param addr_byte 地址字节 (含 R/W 位)
 * @param timeout_ms 超时 (ms)
 * @param start 起始 tick (HAL_GetTick)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int i2c_send_addr(I2C_TypeDef* i2c, uint8_t addr_byte, uint32_t timeout_ms, uint32_t start)
{
    int ret;

    LL_I2C_GenerateStartCondition(i2c);
    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_SB, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;

    LL_I2C_TransmitData8(i2c, addr_byte);
    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_ADDR, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    return MINI_OK;
}

/**
 * @brief 主机写: START + addr(W) + data[len] + STOP
 * @param i2c I2C 外设指针
 * @param cfg 设备配置指针
 * @param tx 发送数据缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int i2c_master_write(I2C_TypeDef* i2c, const struct hal_i2c_device_config* cfg, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    size_t   i;
    int      ret;

    ret = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    ret = i2c_send_addr(i2c, i2c_addr_byte(cfg, 0), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_ClearFlag_ADDR(i2c);

    for (i = 0; i < len; i++)
    {
        ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_TXE, timeout_ms, start);
        if (ret != MINI_OK)
            return ret;
        LL_I2C_TransmitData8(i2c, tx[i]);
    }

    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;

    LL_I2C_GenerateStopCondition(i2c);
    return MINI_OK;
}

/**
 * @brief 读相位 (ADDR 已发出): len==1 / ==2(POS+BTF) / >=3(末 3 字节 BTF)
 * @param i2c I2C 外设指针
 * @param rx 接收缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @param start 起始 tick (HAL_GetTick)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int i2c_read_phase(I2C_TypeDef* i2c, uint8_t* rx, size_t len, uint32_t timeout_ms, uint32_t start)
{
    size_t i;
    int    ret;

    if (len == 1)
    {
        LL_I2C_ClearFlag_ADDR(i2c);
        LL_I2C_GenerateStopCondition(i2c);
        ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_RXNE, timeout_ms, start);
        if (ret != MINI_OK)
            return ret;
        rx[0] = LL_I2C_ReceiveData8(i2c);
        return MINI_OK;
    }

    if (len == 2)
    {
        LL_I2C_EnableBitPOS(i2c);
        LL_I2C_ClearFlag_ADDR(i2c);
        LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
        ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
        if (ret != MINI_OK)
        {
            LL_I2C_DisableBitPOS(i2c);
            return ret;
        }
        LL_I2C_GenerateStopCondition(i2c);
        rx[0] = LL_I2C_ReceiveData8(i2c);
        rx[1] = LL_I2C_ReceiveData8(i2c);
        LL_I2C_DisableBitPOS(i2c);
        return MINI_OK;
    }

    LL_I2C_ClearFlag_ADDR(i2c);
    for (i = 0; i < len - 3U; i++)
    {
        ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_RXNE, timeout_ms, start);
        if (ret != MINI_OK)
            return ret;
        rx[i] = LL_I2C_ReceiveData8(i2c);
    }
    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
    rx[len - 3U] = LL_I2C_ReceiveData8(i2c);
    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_GenerateStopCondition(i2c);
    rx[len - 2U] = LL_I2C_ReceiveData8(i2c);
    rx[len - 1U] = LL_I2C_ReceiveData8(i2c);
    return MINI_OK;
}

/**
 * @brief 主机读: START + addr(R) + data[len] + STOP
 * @param i2c I2C 外设指针
 * @param cfg 设备配置指针
 * @param rx 接收缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int i2c_master_read(I2C_TypeDef* i2c, const struct hal_i2c_device_config* cfg, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int      ret;

    ret = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    LL_I2C_AcknowledgeNextData(i2c, (len == 1) ? LL_I2C_NACK : LL_I2C_ACK);
    ret = i2c_send_addr(i2c, i2c_addr_byte(cfg, 1), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    return i2c_read_phase(i2c, rx, len, timeout_ms, start);
}

/**
 * @brief 写后读: 写完用 Repeated START 再读 (不发中间 STOP)
 * @param i2c I2C 外设指针
 * @param cfg 设备配置指针
 * @param tx 写阶段数据缓冲
 * @param rx 读阶段接收缓冲
 * @param len 读写字节数 (写 len 字节后再读 len 字节)
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int i2c_master_write_then_read(I2C_TypeDef* i2c, const struct hal_i2c_device_config* cfg, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    size_t   i;
    int      ret;

    ret = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    ret = i2c_send_addr(i2c, i2c_addr_byte(cfg, 0), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_ClearFlag_ADDR(i2c);

    for (i = 0; i < len; i++)
    {
        ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_TXE, timeout_ms, start);
        if (ret != MINI_OK)
            return ret;
        LL_I2C_TransmitData8(i2c, tx[i]);
    }
    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    if (ret != MINI_OK)
        return ret;

    LL_I2C_AcknowledgeNextData(i2c, (len == 1) ? LL_I2C_NACK : LL_I2C_ACK);
    ret = i2c_send_addr(i2c, i2c_addr_byte(cfg, 1), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    return i2c_read_phase(i2c, rx, len, timeout_ms, start);
}

/**
 * @brief 初始化 I2C 设备对象 (绑定 host 与设备配置)
 * @param pdev I2C 设备指针
 * @param host 总线主机对象指针
 * @param dev_cfg 设备配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_init(struct hal_i2c_dev* pdev, struct hal_i2c_bus_host* host, const struct hal_i2c_device_config* dev_cfg)
{
    if (!pdev || !host || !dev_cfg)
        return MINI_ERR_INVAL;

    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->ctlr = host;
    pdev->cfg  = *dev_cfg;
    return MINI_OK;
}

/**
 * @brief 释放 I2C 设备 (若已打开则先 hw_close)
 * @param pdev I2C 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_deinit(struct hal_i2c_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;

    if (pdev->hw_open)
        (void)hal_i2c_dev_hw_close(pdev);

    pdev->ctlr = NULL;
    COMPAT_MEM_SET(&pdev->cfg, 0, sizeof(pdev->cfg));
    return MINI_OK;
}

/**
 * @brief 初始化 I2C 总线主机 (时钟/GPIO/DMA 静态配置)
 * @param host 总线主机对象指针
 * @param hw_idx HW slot 索引
 * @param cfg 总线配置 (硬件直投)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_NODEV
 */
int hal_i2c_bus_host_init(struct hal_i2c_bus_host* host, int hw_idx, const struct hal_i2c_bus_config* cfg)
{
    int ret;

    if (!host || !cfg || hw_idx < 0 || hw_idx >= HAL_I2C_HOST_MAX)
        return MINI_ERR_INVAL;
    if (host->bus_ready)
        return MINI_OK;
    if (!cfg->i2c)
        return MINI_ERR_NODEV;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;
    if (host->cfg.max_transfer_sz == 0)
        host->cfg.max_transfer_sz = HAL_I2C_MAX_XFER;
    else if (host->cfg.max_transfer_sz > HAL_I2C_MAX_XFER)
        host->cfg.max_transfer_sz = HAL_I2C_MAX_XFER;

    LL_APB1_GRP1_EnableClock(cfg->i2c_clk_periph);

    ret = hal_i2c_gpio_config(&cfg->scl);
    if (ret != MINI_OK)
        return ret;
    ret = hal_i2c_gpio_config(&cfg->sda);
    if (ret != MINI_OK)
        return ret;

    if (cfg->dma_tx.dma_enable && cfg->dma_tx.dma_handle)
        hal_i2c_dma_init(&cfg->dma_tx, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    if (cfg->dma_rx.dma_enable && cfg->dma_rx.dma_handle)
        hal_i2c_dma_init(&cfg->dma_rx, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

    host->i2c       = cfg->i2c;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return MINI_OK;
}

/**
 * @brief 释放 I2C 总线主机 (关 I2C、复位 SCL/SDA 引脚)
 * @param host 总线主机对象指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2c_bus_host_deinit(struct hal_i2c_bus_host* host)
{
    if (!host)
        return MINI_ERR_INVAL;
    if (!host->bus_ready)
        return MINI_OK;

    if (host->i2c)
        LL_I2C_Disable((I2C_TypeDef*)host->i2c);

    /* 引脚复位为 analog  */
    hal_i2c_gpio_reset(&host->cfg.scl);
    hal_i2c_gpio_reset(&host->cfg.sda);

    host->bus_ready = false;
    host->hw_inited = false;
    return MINI_OK;
}

/**
 * @brief 将 client 配置应用到 I2C 外设并重新初始化
 * @param host I2C host 指针
 * @param dev_cfg 设备配置指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL, LL 初始化失败返回 MINI_ERR_IO
 */
static int hal_i2c_apply_dev_cfg(struct hal_i2c_bus_host* host, const struct hal_i2c_device_config* dev_cfg)
{
    LL_I2C_InitTypeDef init = {0};
    I2C_TypeDef*       i2c;

    if (!host || !dev_cfg || !host->i2c)
        return MINI_ERR_INVAL;

    i2c = (I2C_TypeDef*)host->i2c;
    LL_I2C_Disable(i2c);

    init.PeripheralMode  = host->cfg.mode ? host->cfg.mode : LL_I2C_MODE_I2C;
    init.ClockSpeed      = dev_cfg->clock_speed_hz;
    init.DutyCycle       = dev_cfg->duty_cycle ? dev_cfg->duty_cycle: LL_I2C_DUTYCYCLE_2;
    init.OwnAddress1     = dev_cfg->own_address;
    init.TypeAcknowledge = dev_cfg->ack_enable ? LL_I2C_ACK : LL_I2C_NACK;
    /* 主机 7-bit 寻址; addr_width: 0=7bit, 1=10bit(未实现) */
    init.OwnAddrSize     = dev_cfg->addr_width ? LL_I2C_OWNADDRESS1_10BIT : LL_I2C_OWNADDRESS1_7BIT;

    if (LL_I2C_Init(i2c, &init) != SUCCESS)
        return MINI_ERR_IO;

    LL_I2C_Enable(i2c);
    host->active_cfg = *dev_cfg;
    return MINI_OK;
}

/**
 * @brief 打开 I2C 设备 (应用 dev 配置, 仅支持 7-bit 寻址)
 * @param dev I2C 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL / MINI_ERR_NODEV / MINI_ERR_NOTSUPP
 */
int hal_i2c_dev_hw_open(struct hal_i2c_dev* dev)
{
    struct hal_i2c_bus_host* host;
    int                      ret;

    if (!dev || !dev->ctlr || !dev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!dev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (dev->hw_open)
        return MINI_OK;

    host = dev->ctlr;
    if (dev->cfg.addr_width != 0)
        return MINI_ERR_NOTSUPP; /* 10-bit 主机寻址未实现 */

    ret  = hal_i2c_apply_dev_cfg(host, &dev->cfg);
    if (ret != MINI_OK)
        return ret;

    host->hw_inited = true;
    host->ref_count++;
    dev->hw_open = 1;
    return MINI_OK;
}

/**
 * @brief 关闭 I2C 设备 (递减引用, ref=0 时关 I2C)
 * @param pdev I2C 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_i2c_dev_hw_close(struct hal_i2c_dev* pdev)
{
    struct hal_i2c_bus_host* host;

    if (!pdev || !pdev->ctlr || !pdev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!pdev->hw_open)
        return MINI_OK;

    host = pdev->ctlr;
    if (host->ref_count > 0)
        host->ref_count--;
    if (host->ref_count == 0)
    {
        LL_I2C_Disable((I2C_TypeDef*)host->i2c);
        host->hw_inited = false;
    }

    pdev->hw_open = 0;
    return MINI_OK;
}

/**
 * @brief 传输前公共校验 + 按需应用 client 配置
 * @param pdev I2C 设备指针
 * @param len 传输字节数
 * @return 成功返回 MINI_OK, 失败返回负数错误码
 */
static int hal_i2c_prepare(struct hal_i2c_dev* pdev, size_t len)
{
    int ret;

    if (!pdev || !pdev->ctlr || !pdev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (len == 0 || len > pdev->ctlr->cfg.max_transfer_sz || len > HAL_I2C_MAX_XFER)
        return MINI_ERR_INVAL;

    if (COMPAT_MEM_COPY(&pdev->ctlr->active_cfg, &pdev->cfg, sizeof(pdev->cfg)) != 0)
    {
        ret = hal_i2c_apply_dev_cfg(pdev->ctlr, &pdev->cfg);
        if (ret != MINI_OK)
            return ret;
    }
    return MINI_OK;
}

/**
 * @brief I2C 主机写 (START + addr(W) + data + STOP)
 * @param pdev I2C 设备指针
 * @param tx 发送数据缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    int ret;

    if (!tx)
        return MINI_ERR_INVAL;

    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    return i2c_master_write((I2C_TypeDef*)pdev->ctlr->i2c, &pdev->cfg, tx, len, timeout_ms);
}

/**
 * @brief I2C 主机读 (START + addr(R) + data + STOP)
 * @param pdev I2C 设备指针
 * @param rx 接收缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    int ret;

    if (!rx)
        return MINI_ERR_INVAL;

    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    return i2c_master_read((I2C_TypeDef*)pdev->ctlr->i2c, &pdev->cfg, rx, len, timeout_ms);
}

/**
 * @brief I2C 同步传输 (纯写/纯读/写后读 Repeated START)
 * @param pdev I2C 设备指针
 * @param tx 写阶段缓冲 (可为 NULL)
 * @param rx 读阶段缓冲 (可为 NULL)
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_sync(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    int ret;

    if (!tx && !rx)
        return MINI_ERR_INVAL;

    /* 纯写 / 纯读走专用路径 */
    if (tx && !rx)
        return hal_i2c_write(pdev, tx, len, timeout_ms);
    if (!tx && rx)
        return hal_i2c_read(pdev, rx, len, timeout_ms);

    /* 先写后读 (Repeated START) */
    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    return i2c_master_write_then_read((I2C_TypeDef*)pdev->ctlr->i2c, &pdev->cfg, tx, rx, len, timeout_ms);
}

/**
 * @brief 检查指定 DMA 流是否传输完成
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号 (0..7)
 * @return true 表示 TC 标志已置位, false 表示未完成或 stream 非法
 */
static bool hal_i2c_dma_is_active_tc(DMA_TypeDef* dma, uint32_t stream)
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
 * @brief 清除指定 DMA 流的传输完成 (TC) 标志
 * @param dma DMA 控制器指针
 * @param stream DMA 流编号 (0..7)
 */
static void hal_i2c_dma_clear_tc(DMA_TypeDef* dma, uint32_t stream)
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
 * @brief 按配置初始化 I2C DMA 流
 * @param cfg DMA 配置指针
 * @param direction DMA 传输方向 (LL_DMA_DIRECTION_*)
 */
static void hal_i2c_dma_init(const struct hal_i2c_dma_config* cfg, uint32_t direction)
{
    LL_DMA_InitTypeDef init = {0};

    LL_DMA_DisableStream((DMA_TypeDef*)cfg->dma_handle, cfg->dma_stream);

    init.PeriphOrM2MSrcAddress  = 0;
    init.MemoryOrM2MDstAddress  = 0;
    init.Direction              = direction;
    init.Mode                   = cfg->dma_mode;
    init.PeriphOrM2MSrcIncMode  = cfg->dma_periph_inc;
    init.MemoryOrM2MDstIncMode  = cfg->dma_mem_inc;
    init.PeriphOrM2MSrcDataSize = cfg->dma_periph_data_size;
    init.MemoryOrM2MDstDataSize = cfg->dma_memory_size;
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
 * @brief I2C DMA 主机写
 * @param pdev I2C 设备指针
 * @param tx 发送数据缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_dma_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    DMA_TypeDef* dma;
    I2C_TypeDef* i2c;
    uint32_t     stream;
    uint32_t     start;
    int          ret;

    if (!pdev || !tx || !pdev->ctlr || !pdev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (!pdev->ctlr->cfg.dma_tx.dma_enable)
        return MINI_ERR_NOTSUPP;

    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    dma    = (DMA_TypeDef*)pdev->ctlr->cfg.dma_tx.dma_handle;
    stream = pdev->ctlr->cfg.dma_tx.dma_stream;
    i2c    = (I2C_TypeDef*)pdev->ctlr->i2c;
    if (!dma)
        return MINI_ERR_IO;

    start = HAL_GetTick();
    ret   = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    ret = i2c_send_addr(i2c, i2c_addr_byte(&pdev->cfg, 0), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_ClearFlag_ADDR(i2c);

    LL_DMA_DisableStream(dma, stream);
    LL_DMA_ConfigAddresses(dma, stream, (uint32_t)tx, (uint32_t)&i2c->DR, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetDataLength(dma, stream, len);
    hal_i2c_dma_clear_tc(dma, stream);

    LL_I2C_EnableDMAReq_TX(i2c);
    LL_DMA_EnableStream(dma, stream);

    while (!hal_i2c_dma_is_active_tc(dma, stream))
    {
        if (LL_I2C_IsActiveFlag_AF(i2c))
        {
            LL_I2C_ClearFlag_AF(i2c);
            LL_DMA_DisableStream(dma, stream);
            LL_I2C_DisableDMAReq_TX(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_IO;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            LL_DMA_DisableStream(dma, stream);
            LL_I2C_DisableDMAReq_TX(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_TIMEOUT;
        }
    }

    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    hal_i2c_dma_clear_tc(dma, stream);
    LL_DMA_DisableStream(dma, stream);
    LL_I2C_DisableDMAReq_TX(i2c);
    LL_I2C_GenerateStopCondition(i2c);
    return ret;
}

/**
 * @brief I2C DMA 主机读 (len==1 时回退轮询)
 * @param pdev I2C 设备指针
 * @param rx 接收缓冲
 * @param len 字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_dma_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len,uint32_t timeout_ms)
{
    DMA_TypeDef* dma;
    I2C_TypeDef* i2c;
    uint32_t     stream;
    uint32_t     start;
    int          ret;

    if (!pdev || !rx || !pdev->ctlr || !pdev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (!pdev->ctlr->cfg.dma_rx.dma_enable)
        return MINI_ERR_NOTSUPP;

    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    /* 单字节 DMA 读时序特殊, 回退轮询路径 */
    if (len == 1)
        return i2c_master_read((I2C_TypeDef*)pdev->ctlr->i2c, &pdev->cfg, rx, len, timeout_ms);

    dma    = (DMA_TypeDef*)pdev->ctlr->cfg.dma_rx.dma_handle;
    stream = pdev->ctlr->cfg.dma_rx.dma_stream;
    i2c    = (I2C_TypeDef*)pdev->ctlr->i2c;
    if (!dma)
        return MINI_ERR_IO;

    start = HAL_GetTick();
    ret   = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);
    /* STM32F4: 末字节 NACK+STOP 由 LAST 位配合 DMA 完成 */
    LL_I2C_EnableLastDMA(i2c);

    ret = i2c_send_addr(i2c, i2c_addr_byte(&pdev->cfg, 1), timeout_ms, start);
    if (ret != MINI_OK)
    {
        LL_I2C_DisableLastDMA(i2c);
        return ret;
    }

    LL_DMA_DisableStream(dma, stream);
    LL_DMA_ConfigAddresses(dma, stream, (uint32_t)&i2c->DR, (uint32_t)rx, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetDataLength(dma, stream, len);
    hal_i2c_dma_clear_tc(dma, stream);

    LL_I2C_EnableDMAReq_RX(i2c);
    LL_DMA_EnableStream(dma, stream);
    LL_I2C_ClearFlag_ADDR(i2c);

    while (!hal_i2c_dma_is_active_tc(dma, stream))
    {
        if (LL_I2C_IsActiveFlag_AF(i2c))
        {
            LL_I2C_ClearFlag_AF(i2c);
            LL_DMA_DisableStream(dma, stream);
            LL_I2C_DisableDMAReq_RX(i2c);
            LL_I2C_DisableLastDMA(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_IO;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            LL_DMA_DisableStream(dma, stream);
            LL_I2C_DisableDMAReq_RX(i2c);
            LL_I2C_DisableLastDMA(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_TIMEOUT;
        }
    }

    hal_i2c_dma_clear_tc(dma, stream);
    LL_DMA_DisableStream(dma, stream);
    LL_I2C_DisableDMAReq_RX(i2c);
    LL_I2C_DisableLastDMA(i2c);
    LL_I2C_GenerateStopCondition(i2c);
    return MINI_OK;
}

/**
 * @brief I2C DMA 写后读 (Repeated START, 中间无 STOP)
 * @param pdev I2C 设备指针
 * @param tx 写阶段缓冲
 * @param rx 读阶段缓冲
 * @param len 读写字节数
 * @param timeout_ms 超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_i2c_dma_write_then_read(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    DMA_TypeDef* dma_tx;
    DMA_TypeDef* dma_rx;
    I2C_TypeDef* i2c;
    uint32_t     stream_tx;
    uint32_t     stream_rx;
    uint32_t     start;
    int          ret;

    if (!pdev || !tx || !rx || !pdev->ctlr || !pdev->ctlr->i2c)
        return MINI_ERR_INVAL;
    if (!pdev->hw_open || !pdev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (!pdev->ctlr->cfg.dma_tx.dma_enable)
        return MINI_ERR_NOTSUPP;
    /* len>=2 读相位走 DMA; len==1 读相位回退 poll, 只需 TX DMA */
    if (len > 1U && !pdev->ctlr->cfg.dma_rx.dma_enable)
        return MINI_ERR_NOTSUPP;

    ret = hal_i2c_prepare(pdev, len);
    if (ret != MINI_OK)
        return ret;

    dma_tx    = (DMA_TypeDef*)pdev->ctlr->cfg.dma_tx.dma_handle;
    stream_tx = pdev->ctlr->cfg.dma_tx.dma_stream;
    i2c       = (I2C_TypeDef*)pdev->ctlr->i2c;
    if (!dma_tx)
        return MINI_ERR_IO;

    start = HAL_GetTick();
    ret   = wait_not_busy(i2c, timeout_ms);
    if (ret != MINI_OK)
        return ret;

    /* ---- 写相位 (DMA, 不发 STOP) ---- */
    ret = i2c_send_addr(i2c, i2c_addr_byte(&pdev->cfg, 0), timeout_ms, start);
    if (ret != MINI_OK)
        return ret;
    LL_I2C_ClearFlag_ADDR(i2c);

    LL_DMA_DisableStream(dma_tx, stream_tx);
    LL_DMA_ConfigAddresses(dma_tx, stream_tx, (uint32_t)tx, (uint32_t)&i2c->DR,
                           LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetDataLength(dma_tx, stream_tx, len);
    hal_i2c_dma_clear_tc(dma_tx, stream_tx);

    LL_I2C_EnableDMAReq_TX(i2c);
    LL_DMA_EnableStream(dma_tx, stream_tx);

    while (!hal_i2c_dma_is_active_tc(dma_tx, stream_tx))
    {
        if (LL_I2C_IsActiveFlag_AF(i2c))
        {
            LL_I2C_ClearFlag_AF(i2c);
            LL_DMA_DisableStream(dma_tx, stream_tx);
            LL_I2C_DisableDMAReq_TX(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_IO;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            LL_DMA_DisableStream(dma_tx, stream_tx);
            LL_I2C_DisableDMAReq_TX(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_TIMEOUT;
        }
    }

    ret = wait_flag_set(i2c, LL_I2C_IsActiveFlag_BTF, timeout_ms, start);
    hal_i2c_dma_clear_tc(dma_tx, stream_tx);
    LL_DMA_DisableStream(dma_tx, stream_tx);
    LL_I2C_DisableDMAReq_TX(i2c);
    if (ret != MINI_OK)
    {
        LL_I2C_GenerateStopCondition(i2c);
        return ret;
    }

    /* ---- 读相位 (Repeated START, 中间无 STOP) ---- */
    if (len == 1U)
    {
        LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
        ret = i2c_send_addr(i2c, i2c_addr_byte(&pdev->cfg, 1), timeout_ms, start);
        if (ret != MINI_OK)
            return ret;
        return i2c_read_phase(i2c, rx, len, timeout_ms, start);
    }

    dma_rx    = (DMA_TypeDef*)pdev->ctlr->cfg.dma_rx.dma_handle;
    stream_rx = pdev->ctlr->cfg.dma_rx.dma_stream;
    if (!dma_rx)
    {
        LL_I2C_GenerateStopCondition(i2c);
        return MINI_ERR_IO;
    }

    LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);
    LL_I2C_EnableLastDMA(i2c);

    ret = i2c_send_addr(i2c, i2c_addr_byte(&pdev->cfg, 1), timeout_ms, start);
    if (ret != MINI_OK)
    {
        LL_I2C_DisableLastDMA(i2c);
        return ret;
    }

    LL_DMA_DisableStream(dma_rx, stream_rx);
    LL_DMA_ConfigAddresses(dma_rx, stream_rx, (uint32_t)&i2c->DR, (uint32_t)rx,
                           LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetDataLength(dma_rx, stream_rx, len);
    hal_i2c_dma_clear_tc(dma_rx, stream_rx);

    LL_I2C_EnableDMAReq_RX(i2c);
    LL_DMA_EnableStream(dma_rx, stream_rx);
    LL_I2C_ClearFlag_ADDR(i2c);

    while (!hal_i2c_dma_is_active_tc(dma_rx, stream_rx))
    {
        if (LL_I2C_IsActiveFlag_AF(i2c))
        {
            LL_I2C_ClearFlag_AF(i2c);
            LL_DMA_DisableStream(dma_rx, stream_rx);
            LL_I2C_DisableDMAReq_RX(i2c);
            LL_I2C_DisableLastDMA(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_IO;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            LL_DMA_DisableStream(dma_rx, stream_rx);
            LL_I2C_DisableDMAReq_RX(i2c);
            LL_I2C_DisableLastDMA(i2c);
            LL_I2C_GenerateStopCondition(i2c);
            return MINI_ERR_TIMEOUT;
        }
    }

    hal_i2c_dma_clear_tc(dma_rx, stream_rx);
    LL_DMA_DisableStream(dma_rx, stream_rx);
    LL_I2C_DisableDMAReq_RX(i2c);
    LL_I2C_DisableLastDMA(i2c);
    LL_I2C_GenerateStopCondition(i2c);
    return MINI_OK;
}
