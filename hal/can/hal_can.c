/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CAN HAL — STM32F4 实现 (bxCAN via HAL_CAN + LL GPIO)
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传。
 * - 厂商句柄嵌入 host->hcan_storage；HAL 回调只有 hcan* 时用 container_of 反查 host
 * - F4 bxCAN 无 DMA；收发走邮箱轮询 / it_enable+NVIC
 * - 自行配置 GPIO AF, 不依赖 CubeMX 生成代码
 */
#include "hal_can.h"
#include "compiler_compat.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"

#define HAL_CAN_SLAVE_FILTER_BANK_START 14U

_Static_assert(sizeof(CAN_HandleTypeDef) <= HAL_CAN_HCAN_STORAGE_SIZE,
               "HAL_CAN_HCAN_STORAGE_SIZE too small for CAN_HandleTypeDef");
_Static_assert(_Alignof(CAN_HandleTypeDef) <= 8,
               "hcan_storage alignment too weak for CAN_HandleTypeDef");

/**
 * @brief 从 HAL CAN 句柄反查所属 host
 * @param hcan 厂商句柄 (指向 host->hcan_storage)
 * @return host 指针; hcan 为空返回 NULL
 */
COMPAT_UNUSED static struct hal_can_bus_host *hal_can_host_from_hcan(CAN_HandleTypeDef *hcan)
{
    if (!hcan)
        return NULL;
    return container_of((struct hal_can_hcan_blob *)(void *)hcan, struct hal_can_bus_host, hcan_storage);
}


/*============================================================================*/
/*                              GPIO AF 直投 helper                            */
/*============================================================================*/
/**
 * @brief 配置 CAN 复用引脚: 时钟使能 + AF 模式 (LL 库直投)
 * @param gpio_cfg 引脚配置 (含 port/pin/clk_bus/af)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
static int hal_can_gpio_config(const struct hal_can_pin_cfg* gpio_cfg)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (!gpio_cfg || !gpio_cfg->port || !gpio_cfg->pin || !gpio_cfg->clk_bus)
        return MINI_ERR_INVAL;

    GPIO_InitStruct.Pin        = gpio_cfg->pin;
    GPIO_InitStruct.Mode       = gpio_cfg->mode ? gpio_cfg->mode : LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed      = gpio_cfg->speed ? gpio_cfg->speed : LL_GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.OutputType = gpio_cfg->output_type ? gpio_cfg->output_type
                                                       : LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull       = gpio_cfg->pull ? gpio_cfg->pull : LL_GPIO_PULL_NO;
    GPIO_InitStruct.Alternate  = gpio_cfg->af;

    LL_AHB1_GRP1_EnableClock(gpio_cfg->clk_bus);
    LL_GPIO_Init((GPIO_TypeDef*)gpio_cfg->port, &GPIO_InitStruct);
    return MINI_OK;
}

/**
 * @brief 复位 CAN 引脚为模拟输入 (安全释放)
 * @param gpio_cfg 引脚配置
 */
static void hal_can_gpio_reset(const struct hal_can_pin_cfg* gpio_cfg)
{
    GPIO_TypeDef* port;

    if (!gpio_cfg || !gpio_cfg->port || !gpio_cfg->pin)
        return;

    port = (GPIO_TypeDef*)gpio_cfg->port;
    LL_GPIO_SetPinMode(port, gpio_cfg->pin, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinPull(port, gpio_cfg->pin, LL_GPIO_PULL_NO);
}

/*============================================================================*/
/*                              Host / Device 管理                             */
/*============================================================================*/
/**
 * @brief 初始化 CAN host (GPIO/时钟/控制器 + 默认 accept-all 过滤器)
 * @param host   host 对象
 * @param hw_idx 硬件实例索引
 * @param cfg    DTSI 直投配置 (不可为 NULL)
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_can_bus_host_init(struct hal_can_bus_host* host, int hw_idx, const struct hal_can_bus_config* cfg)
{
    CAN_HandleTypeDef* hcan;
    int ret;

    if (!host || !cfg || !cfg->can)
        return MINI_ERR_INVAL;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg    = *cfg;
    host->can    = cfg->can;
    host->hw_idx = hw_idx;

    ret = hal_can_gpio_config(&cfg->tx);
    if (ret != MINI_OK)
        return ret;
    ret = hal_can_gpio_config(&cfg->rx);
    if (ret != MINI_OK)
    {
        hal_can_gpio_reset(&cfg->tx);
        return ret;
    }

    LL_APB1_GRP1_EnableClock(cfg->can_clk_periph);
    if (cfg->can == CAN2_BASE)
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_CAN1);

    hcan = (CAN_HandleTypeDef *)(void *)(&(host)->hcan_storage);
    COMPAT_MEM_SET(hcan, 0, sizeof(*hcan));
    hcan->Instance = (CAN_TypeDef*)cfg->can;

    hcan->Init.Prescaler            = cfg->prescaler;
    hcan->Init.Mode                 = cfg->mode;
    hcan->Init.SyncJumpWidth        = cfg->sjw;
    hcan->Init.TimeSeg1             = cfg->bs1;
    hcan->Init.TimeSeg2             = cfg->bs2;
    hcan->Init.TimeTriggeredMode    = cfg->tt_mode ? ENABLE : DISABLE;
    hcan->Init.AutoBusOff           = cfg->auto_bus_off ? ENABLE : DISABLE;
    hcan->Init.AutoWakeUp           = cfg->auto_wakeup ? ENABLE : DISABLE;
    hcan->Init.AutoRetransmission   = cfg->auto_retransmit ? ENABLE : DISABLE;
    hcan->Init.ReceiveFifoLocked    = cfg->rx_fifo_locked ? ENABLE : DISABLE;
    hcan->Init.TransmitFifoPriority = cfg->tx_fifo_prio ? ENABLE : DISABLE;

    if (HAL_CAN_Init(hcan) != HAL_OK)
    {
        hal_can_gpio_reset(&cfg->tx);
        hal_can_gpio_reset(&cfg->rx);
        return MINI_ERR_IO;
    }

    if (HAL_CAN_Start(hcan) != HAL_OK)
    {
        COMPAT_IGNORE_RESULT(HAL_CAN_DeInit(hcan));
        hal_can_gpio_reset(&cfg->tx);
       hal_can_gpio_reset(&cfg->rx);
        return MINI_ERR_IO;
    }

    {
        struct hal_can_filter_config accept_all = {
            .bank = 0,
            .mode = HAL_CAN_FILTER_MODE_MASK,
            .scale = HAL_CAN_FILTER_SCALE_32BIT,
            .fifo = 0,
            .id = 0,
            .mask = 0,
            .ide = 0,
            .rtr = 0,
        };
        if (hal_can_filter_config(host, &accept_all) != MINI_OK)
        {
            COMPAT_IGNORE_RESULT(HAL_CAN_Stop(hcan));
            COMPAT_IGNORE_RESULT(HAL_CAN_DeInit(hcan));
           hal_can_gpio_reset(&cfg->tx);
           hal_can_gpio_reset(&cfg->rx);
            return MINI_ERR_IO;
        }
    }

    if (cfg->it_enable && cfg->irqn >= 0)
    {
        NVIC_SetPriority((IRQn_Type)cfg->irqn, cfg->irq_priority);
        NVIC_EnableIRQ((IRQn_Type)cfg->irqn);
    }

    host->bus_ready = true;
    host->hw_inited = true;
    return MINI_OK;
}

/**
 * @brief 反初始化 CAN host (停控制器、关时钟、复位引脚)
 * @param host host 对象
 * @return 成功返回 MINI_OK, 未初始化返回 MINI_ERR_INVAL
 */
int hal_can_bus_host_deinit(struct hal_can_bus_host* host)
{
    if (!host || !host->hw_inited)
        return MINI_ERR_INVAL;

    if (host->cfg.it_enable && host->cfg.irqn >= 0)
        NVIC_DisableIRQ((IRQn_Type)host->cfg.irqn);

    COMPAT_IGNORE_RESULT(HAL_CAN_Stop((CAN_HandleTypeDef *)(void *)(&(host)->hcan_storage)));
    COMPAT_IGNORE_RESULT(HAL_CAN_DeInit((CAN_HandleTypeDef *)(void *)(&(host)->hcan_storage)));
    LL_APB1_GRP1_DisableClock(host->cfg.can_clk_periph);
   hal_can_gpio_reset(&host->cfg.tx);
   hal_can_gpio_reset(&host->cfg.rx);

    host->bus_ready = false;
    host->hw_inited = false;
    return MINI_OK;
}

/**
 * @brief 绑定设备到 host (不启动硬件)
 * @param dev  设备对象
 * @param host 所属 host
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_can_dev_init(struct hal_can_dev*dev, struct hal_can_bus_host* host)
{
    if (!dev || !host)
        return MINI_ERR_INVAL;
    COMPAT_MEM_SET(dev, 0, sizeof(*dev));
   dev->ctlr = host;
    return MINI_OK;
}

/**
 * @brief 解绑并清零设备对象
 * @param dev 设备对象
 * @return 成功返回 MINI_OK, dev 为空返回 MINI_ERR_INVAL
 */
int hal_can_dev_deinit(struct hal_can_dev*dev)
{
    if (!dev)
        return MINI_ERR_INVAL;
    COMPAT_MEM_SET(dev, 0, sizeof(*dev));
    return MINI_OK;
}

/**
 * @brief 打开 CAN 设备硬件路径 (幂等计数 +1)
 * @param dev 设备对象
 * @return 成功返回 MINI_OK, host 未就绪返回 MINI_ERR_INVAL
 */
int hal_can_dev_hw_open(struct hal_can_dev*dev)
{
    if (!dev || !dev->ctlr || !dev->ctlr->bus_ready)
        return MINI_ERR_INVAL;
   dev->hw_open++;
    return MINI_OK;
}

/**
 * @brief 关闭 CAN 设备硬件路径 (幂等计数 -1)
 * @param dev 设备对象
 * @return 成功返回 MINI_OK, dev 为空返回 MINI_ERR_INVAL
 */
int hal_can_dev_hw_close(struct hal_can_dev*dev)
{
    if (!dev)
        return MINI_ERR_INVAL;
    if (dev->hw_open > 0)
       dev->hw_open--;
    return MINI_OK;
}

/*============================================================================*/
/*                              收发 / 过滤 / 状态                              */
/*============================================================================*/
/**
 * @brief 发送一帧 (经典 CAN, 等待空闲邮箱)
 * @param dev        设备对象
 * @param frame      待发送帧 (拒绝 CAN_ERR_FLAG)
 * @param timeout_ms 等待空闲邮箱超时 (毫秒)
 * @return 成功返回 MINI_OK, 超时 MINI_ERR_TIMEOUT, 失败 VFS_ERR_*
 */
int hal_can_transmit(struct hal_can_dev*dev, const struct can_frame* frame, uint32_t timeout_ms)
{
    CAN_HandleTypeDef* hcan;
    CAN_TxHeaderTypeDef header;
    uint32_t tx_mailbox;
    uint32_t start;

    if (!dev || !dev->ctlr || !frame)
        return MINI_ERR_INVAL;
    if (frame->can_dlc > CAN_MAX_DLEN)
        return MINI_ERR_INVAL;
    if (!dev->ctlr->bus_ready)
        return MINI_ERR_NODEV;
    if (frame->can_id & CAN_ERR_FLAG)
        return MINI_ERR_INVAL;

    hcan = (CAN_HandleTypeDef *)(void *)(&(dev->ctlr)->hcan_storage);
    start = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(hcan) == 0U)
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }

    COMPAT_MEM_SET(&header, 0, sizeof(header));
    if (frame->can_id & CAN_EFF_FLAG)
    {
        header.IDE = CAN_ID_EXT;
        header.ExtId = frame->can_id & CAN_EFF_MASK;
    }
    else
    {
        header.IDE = CAN_ID_STD;
        header.StdId = frame->can_id & CAN_SFF_MASK;
    }
    header.RTR = (frame->can_id & CAN_RTR_FLAG) ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    header.DLC = frame->can_dlc;
    header.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(hcan, &header, (uint8_t*)frame->data, &tx_mailbox) != HAL_OK)
        return MINI_ERR_IO;
    return MINI_OK;
}

/**
 * @brief 从指定 FIFO 接收一帧
 * @param dev        设备对象
 * @param frame      输出帧
 * @param fifo       RX FIFO 编号 (0 / 1)
 * @param timeout_ms 等待有数据超时 (毫秒)
 * @return 成功返回 MINI_OK, 超时 MINI_ERR_TIMEOUT, 失败 VFS_ERR_*
 */
int hal_can_receive(struct hal_can_dev*dev, struct can_frame* frame, uint32_t fifo, uint32_t timeout_ms)
{
    CAN_HandleTypeDef* hcan;
    CAN_RxHeaderTypeDef header;
    uint32_t rx_fifo;
    uint32_t start;

    if (!dev || !dev->ctlr || !frame)
        return MINI_ERR_INVAL;
    if (fifo > 1U)
        return MINI_ERR_INVAL;
    if (!dev->ctlr->bus_ready)
        return MINI_ERR_NODEV;

    hcan = (CAN_HandleTypeDef *)(void *)(&(dev->ctlr)->hcan_storage);
    rx_fifo = (fifo == 0U) ? CAN_RX_FIFO0 : CAN_RX_FIFO1;
    start = HAL_GetTick();
    while (HAL_CAN_GetRxFifoFillLevel(hcan, rx_fifo) == 0U)
    {
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
            return MINI_ERR_TIMEOUT;
    }

    COMPAT_MEM_SET(frame, 0, sizeof(*frame));
    if (HAL_CAN_GetRxMessage(hcan, rx_fifo, &header, frame->data) != HAL_OK)
        return MINI_ERR_IO;

    if (header.IDE == CAN_ID_EXT)
        frame->can_id = (header.ExtId & CAN_EFF_MASK) | CAN_EFF_FLAG;
    else
        frame->can_id = header.StdId & CAN_SFF_MASK;
    if (header.RTR == CAN_RTR_REMOTE)
        frame->can_id |= CAN_RTR_FLAG;
    frame->can_dlc = (uint8_t)(header.DLC > CAN_MAX_DLEN ? CAN_MAX_DLEN : header.DLC);
    return MINI_OK;
}

/**
 * @brief 配置硬件过滤器 (作用于 host/控制器)
 * @param host   host 对象
 * @param filter 过滤器配置
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_can_filter_config(struct hal_can_bus_host* host, const struct hal_can_filter_config* filter)
{
    CAN_FilterTypeDef hal_filter;
    uint32_t id_reg;
    uint32_t mask_reg;

    if (!host || !filter)
        return MINI_ERR_INVAL;
    if (filter->bank >= HAL_CAN_FILTER_MAX || filter->fifo > 1U)
        return MINI_ERR_INVAL;

    COMPAT_MEM_SET(&hal_filter, 0, sizeof(hal_filter));
   hal_filter.FilterBank = filter->bank;
   hal_filter.FilterFIFOAssignment = (filter->fifo == 0U) ? CAN_FILTER_FIFO0 : CAN_FILTER_FIFO1;
   hal_filter.FilterMode = (filter->mode == HAL_CAN_FILTER_MODE_LIST)
                            ? CAN_FILTERMODE_IDLIST : CAN_FILTERMODE_IDMASK;
   hal_filter.FilterScale = (filter->scale == HAL_CAN_FILTER_SCALE_32BIT)
                             ? CAN_FILTERSCALE_32BIT : CAN_FILTERSCALE_16BIT;
   hal_filter.FilterActivation = CAN_FILTER_ENABLE;
   hal_filter.SlaveStartFilterBank = HAL_CAN_SLAVE_FILTER_BANK_START;

    if (filter->scale == HAL_CAN_FILTER_SCALE_32BIT)
    {
        if (filter->ide)
            id_reg = (filter->id & CAN_EFF_MASK) << 3;
        else
            id_reg = (filter->id & CAN_SFF_MASK) << 21;
        if (filter->rtr)
            id_reg |= (1U << 1);
        if (filter->ide)
            id_reg |= (1U << 2);

       hal_filter.FilterIdHigh = (uint16_t)(id_reg >> 16);
       hal_filter.FilterIdLow  = (uint16_t)(id_reg & 0xFFFFU);

        if (filter->mode == HAL_CAN_FILTER_MODE_MASK)
        {
            if (filter->ide)
                mask_reg = (filter->mask & CAN_EFF_MASK) << 3;
            else
                mask_reg = (filter->mask & CAN_SFF_MASK) << 21;
            if (filter->mask != 0U)
                mask_reg |= (1U << 2) | (1U << 1);
        }
        else
            mask_reg = 0;

       hal_filter.FilterMaskIdHigh = (uint16_t)(mask_reg >> 16);
       hal_filter.FilterMaskIdLow  = (uint16_t)(mask_reg & 0xFFFFU);
    }
    else
    {
        uint16_t id0 = (uint16_t)((filter->id & CAN_SFF_MASK) << 5);
        uint16_t id1 = (uint16_t)((filter->mask & CAN_SFF_MASK) << 5);
        if (filter->rtr)
            id0 |= (uint16_t)(1U << 4);
       hal_filter.FilterIdHigh = id0;
       hal_filter.FilterIdLow = id1;
       hal_filter.FilterMaskIdHigh = 0;
       hal_filter.FilterMaskIdLow = 0;
    }

    if (HAL_CAN_ConfigFilter((CAN_HandleTypeDef *)(void *)(&(host)->hcan_storage), &hal_filter) != HAL_OK)
        return MINI_ERR_IO;
    return MINI_OK;
}

/**
 * @brief 查询控制器状态 (读 ESR)
 * @param host      host 对象
 * @param out_state 输出 HAL_CAN_STATE_*
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_can_get_state(struct hal_can_bus_host* host, uint32_t* out_state)
{
    uint32_t esr;

    if (!host || !out_state || !host->hw_inited)
        return MINI_ERR_INVAL;

    if (!host->bus_ready)
    {
        *out_state = HAL_CAN_STATE_STOPPED;
        return MINI_OK;
    }

    esr = ((CAN_HandleTypeDef *)(void *)(&(host)->hcan_storage))->Instance->ESR;
    if (esr & CAN_ESR_BOFF)
        *out_state = HAL_CAN_STATE_BUS_OFF;
    else if (esr & CAN_ESR_EPVF)
        *out_state = HAL_CAN_STATE_ERROR_PASSIVE;
    else
        *out_state = HAL_CAN_STATE_ERROR_ACTIVE;
    return MINI_OK;
}

/**
 * @brief 虚拟/平台 CAN 中断入口 (当前为空实现占位)
 * @param arg     用户参数
 * @param irq_num 逻辑中断号
 * @return MINI_IRQ_ENTRY_NOBOTTOM (无下半部)
 */
int hal_virtual_can_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(arg);
    COMPAT_IGNORE_RESULT(irq_num);
    return MINI_IRQ_ENTRY_NOBOTTOM;
}
