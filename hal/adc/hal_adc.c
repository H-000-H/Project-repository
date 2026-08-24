/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADC HAL — STM32F4 实现 (LL 库直投)
 *
 * 支持: 单通道/多通道扫描, 软件/外部触发, 单次/连续模式, DMA 传输。
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 LL 库。
 */
#include "hal_adc.h"
#include "stm32f4xx_ll_adc.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_dma.h"
#include "buffer.h"
#include "system_log.h"
#include "interrupt.h"
/**< 硬件直投层私有宏：将通道绝对数量转换为 STM32F4 的 SQR1 寄存器长度掩码 */
#define __HAL_ADC_F4_CH_NUM_TO_REG(x)    (((x) - 1) << 20)
#ifndef DTS_DMA_BUFFER_SIZE
#define DMA_BUFFER_SIZE 1024
#endif

/**< ADC1 设备实例由 vfs_adc_probe 在 s_host_pool 中分配, ISR 通过 VIRQ 表 arg 获取 */

/**< 硬件直投层私有高速缓存表，消除热路径下的二次间接寻址 */
#ifndef DTS_HAL_ADC_INSTANCE_MAX
#define DTS_HAL_ADC_INSTANCE_MAX 3 /**< 兜底:f4架构共有 ADC1, ADC2, ADC3 三个独立外设 */
#endif
static ADC_TypeDef *g_adc_fast_tbl[DTS_HAL_ADC_INSTANCE_MAX] = {NULL};

/*===========================================================================================================================================================*/
/*结构体和全局变量定义*/
/*===========================================================================================================================================================*/

/**
 * @brief 配置GPIO的模拟输入功能
 * @param gpio GPIO配置结构体
 * @return MINI_OK 成功, MINI_ERR_INVAL 参数错误
 */
COMPAT_STATIC_INLINE int hal_adc_config_gpio_pin(hal_adc_gpio_config* gpio)
{
    if (!gpio)
        return MINI_ERR_INVAL;

    GPIO_TypeDef* port = (GPIO_TypeDef*)gpio->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    GPIO_InitStruct.Pin        = gpio->pin;
    GPIO_InitStruct.Mode       = gpio->mode;
    GPIO_InitStruct.Pull       = gpio->pull;
    GPIO_InitStruct.Speed      = gpio->speed;
    GPIO_InitStruct.OutputType = gpio->output_type;
    
    /**<只有当模式是复用(AF)时，才去检查并配置复用寄存器*/
    if (gpio->mode == LL_GPIO_MODE_ALTERNATE) 
    {
        if (!gpio->af) return MINI_ERR_INVAL;
        GPIO_InitStruct.Alternate = gpio->af;
    }

    LL_AHB1_GRP1_EnableClock(gpio->clk_bus);
    if (LL_GPIO_Init(port, &GPIO_InitStruct) != SUCCESS)
        return MINI_ERR_INVAL;

    return MINI_OK;
}

/**
 * @brief DMA 静态参数一次性配置 (hal_adc_init 时调用, start 路径只设长度+启停)
 * @param pdev ADC 设备指针 (含 host DMA 配置与采样缓冲)
 * @note  采用 LL_DMA_InitTypeDef + LL_DMA_Init 批量初始化范式 (同 LL_ADC_Init)。
 *        channel/direction/priority/mode/inc/size/地址/长度 来自 DTS 且永不变。
 *        dma_enable=0 时由调用方跳过。
 */
static void hal_adc_dma_init(hal_adc_device* pdev)
{
    LL_DMA_InitTypeDef init       = {0};
    ADC_TypeDef*       adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
    DMA_TypeDef*        dma        = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
    uint32_t            stream     = pdev->host->dma_cfg.dma_stream;

    /** STM32F4 要求配置前 stream disable */
    LL_DMA_DisableStream(dma, stream);

    init.PeriphOrM2MSrcAddress   = LL_ADC_DMA_GetRegAddr(adc_handle, (uint32_t)pdev->host->config.dma_reg_mode);
    init.MemoryOrM2MDstAddress   = (uint32_t)pdev->host->private_cfg->dma_raw_data_buf;
    init.Direction               = pdev->host->dma_cfg.dma_direction;
    init.Mode                    = pdev->host->dma_cfg.dma_mode;
    init.PeriphOrM2MSrcIncMode   = pdev->host->dma_cfg.dma_periph_inc;
    init.MemoryOrM2MDstIncMode   = pdev->host->dma_cfg.dma_mem_inc;
    init.PeriphOrM2MSrcDataSize  = pdev->host->dma_cfg.dma_periph_data_size;
    init.MemoryOrM2MDstDataSize  = pdev->host->dma_cfg.dma_memory_size;
    init.NbData                  = DMA_BUFFER_SIZE;
    init.Channel                 = pdev->host->dma_cfg.dma_channel;
    init.Priority                = pdev->host->dma_cfg.dma_priority;
    init.FIFOMode                = pdev->host->dma_cfg.dma_fifo_mode;
    init.FIFOThreshold           = pdev->host->dma_cfg.dma_fifo_threshold;
    init.MemBurst                = pdev->host->dma_cfg.dma_mem_burst;
    init.PeriphBurst             = pdev->host->dma_cfg.dma_periph_burst;
    LL_DMA_Init(dma, stream, &init);
}

/*===========================================================================================================================================================*/
/*设备初始化与销毁*/
/*===========================================================================================================================================================*/

/**
 * @brief 初始化 ADC 设备对象 (绑定 host 与平台私有配置)
 * @param pdev ADC 设备指针
 * @param unique_cfg 平台私有配置指针
 * @param host ADC host 配置指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_device_init(hal_adc_device* pdev, hal_adc_platform_unique_config* unique_cfg, hal_adc_host_config* host)
{
    if (!pdev || !unique_cfg || !host)
        return MINI_ERR_INVAL;

    pdev->host   = host;
    pdev->unique = unique_cfg;
    return MINI_OK;
}

/**
 * @brief 释放 ADC 设备对象 (清空 host/unique 引用)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK
 */
int hal_adc_device_deinit(hal_adc_device* pdev)
{
    if (!pdev)
        return MINI_OK;

    pdev->unique = NULL;
    pdev->host   = NULL;
    return MINI_OK;
}
/**
 * @brief 初始化 ADC 硬件 (LL_ADC/REG/COMMON/GPIO/DMA 静态配置)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_init(hal_adc_device* pdev)
{
    if (!pdev || !pdev->host->adc_handle || !pdev->host->multi_cfg || !pdev->host->channels)
        return MINI_ERR_INVAL;

    /**< 边界检查：防止 DTS 映射层传入的物理设备索引越界 */
    if (pdev->host->dev_index >= DTS_HAL_ADC_INSTANCE_MAX)
        return MINI_ERR_INVAL;

    LL_ADC_InitTypeDef ADC_InitStruct = {0};
    ADC_InitStruct.DataAlignment                        = pdev->host->config.align;
    ADC_InitStruct.Resolution                           = pdev->host->config.resolution;
    ADC_InitStruct.SequencersScanMode                   = pdev->host->config.sequencer_mode;

    LL_ADC_REG_InitTypeDef ADC_REG_InitStruct = {0};
    ADC_REG_InitStruct.TriggerSource                    = pdev->host->config.trigger_src;
    ADC_REG_InitStruct.ContinuousMode                   = pdev->host->config.continuous_mode;
    ADC_REG_InitStruct.DMATransfer                      = pdev->host->config.dma_mode;
    ADC_REG_InitStruct.SequencerLength                  = pdev->host->config.SequencerLength;
    ADC_REG_InitStruct.SequencerDiscont                 = pdev->host->config.SequencerDiscont;

    LL_ADC_CommonInitTypeDef ADC_CommonInitStruct = {0};
    ADC_CommonInitStruct.CommonClock                    = pdev->host->multi_cfg->common_clock;
    ADC_CommonInitStruct.MultiDMATransfer               = pdev->host->multi_cfg->multi_dma;
    ADC_CommonInitStruct.Multimode                      = pdev->host->multi_cfg->multimode;
    ADC_CommonInitStruct.MultiTwoSamplingDelay          = pdev->host->multi_cfg->sampling_delay;
    
    LL_APB2_GRP1_EnableClock(pdev->host->config.adc_clk_bus);
    
    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;

    LL_ADC_Init(adc_handle, &ADC_InitStruct);
    LL_ADC_REG_Init(adc_handle, &ADC_REG_InitStruct);
    LL_ADC_REG_SetFlagEndOfConversion(adc_handle, pdev->host->config.EOC_flag);
    LL_ADC_CommonInit(__LL_ADC_COMMON_INSTANCE(adc_handle), &ADC_CommonInitStruct);

    /**
     * @brief pdev->host->config.channel_num 在 dts 解析层直接映射为自然数(1, 2, 3...)
     */
    uint32_t convert_count = (uint32_t)pdev->host->config.channel_num; 
    if (convert_count == 0) convert_count = 1; 

    for (uint32_t i = 0; i < convert_count; i++) 
    {
        LL_ADC_REG_SetSequencerRanks(adc_handle, pdev->host->channels[i].rank, pdev->host->channels[i].channel_id);
        LL_ADC_SetChannelSamplingTime(adc_handle, pdev->host->channels[i].channel_id, pdev->host->channels[i].sample_time);
    }

    if (hal_adc_config_gpio_pin(&pdev->host->gpio_cfg) != MINI_OK)
        return MINI_ERR_INVAL;

    if (pdev->host->config.internal_ch_enable) 
        /**<降低读取内部电量的基准,并且开启直读VBAT */
        LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(adc_handle), (uint32_t)pdev->host->config.internal_ch_select);
    
    /**< 初始化阶段完成物理基地址 */
    g_adc_fast_tbl[pdev->host->dev_index] = adc_handle;

    /**< 同步刷新上下文控制句柄的总数 */
    pdev->host->channel_count = convert_count;

    /**< DMA 静态参数一次性配置: dma_enable=0 时跳过 */
    if (pdev->host->dma_cfg.dma_enable)
        hal_adc_dma_init(pdev);

    return MINI_OK;
}

/**
 * @brief 完全去初始化 ADC 外设 (Disable/DeInit/关时钟/复位 GPIO)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_deinit_all_adcx(hal_adc_device *pdev)
{
    if (!pdev || !pdev->host->adc_handle) 
        return MINI_ERR_INVAL;

    if (pdev->host->dev_index >= DTS_HAL_ADC_INSTANCE_MAX)
        return MINI_ERR_INVAL;

    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
    
    if (LL_ADC_IsEnabled(adc_handle) != 0)
        LL_ADC_Disable(adc_handle);
    
    if (LL_ADC_DeInit(adc_handle) != SUCCESS)
        return MINI_ERR_INVAL;

    LL_APB2_GRP1_DisableClock(pdev->host->config.adc_clk_bus);
    
    /**< 恢复引脚状态为浮空输入 */
    if (pdev->host->gpio_cfg.port)
    {
        LL_GPIO_SetPinMode((GPIO_TypeDef*)pdev->host->gpio_cfg.port, pdev->host->gpio_cfg.pin, pdev->host->gpio_cfg.mode);
        LL_GPIO_SetPinPull((GPIO_TypeDef*)pdev->host->gpio_cfg.port, pdev->host->gpio_cfg.pin, pdev->host->gpio_cfg.pull);
    }
    
    /**< 释放高速直投表，阻断野指针生存周期 */
    g_adc_fast_tbl[pdev->host->dev_index] = NULL;

    pdev->host->channel_count = 0;
    pdev->host->config.channel_num = 0;
    
    return MINI_OK;
}

/**
 * @brief 从序列中移除指定通道并重组 Rank
 * @param pdev ADC 设备指针
 * @param channel_id 待移除通道 ID
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_deinit_adcx_channel(hal_adc_device *pdev, uint32_t channel_id)
{
    if (!pdev || !pdev->host->adc_handle || !pdev->host->channels)
        return MINI_ERR_INVAL;

    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
    uint32_t current_count = (uint32_t)pdev->host->config.channel_num;

    if (current_count <= 1) 
    {
        pdev->host->config.channel_num = 0;
        pdev->host->channel_count = 0;
        if (LL_ADC_IsEnabled(adc_handle))
            LL_ADC_Disable(adc_handle);
        return MINI_OK;
    }

    int target_index = -1;
    for (uint32_t i = 0; i < current_count; i++) 
    {
        if (pdev->host->channels[i].channel_id == channel_id) 
        {
            target_index = (int)i;
            break;
        }
    }

    if (target_index == -1)
        return MINI_ERR_INVAL; 

    uint32_t is_enabled = LL_ADC_IsEnabled(adc_handle);
    if (is_enabled) 
        LL_ADC_Disable(adc_handle); 

    /**< 重组通道将后一位移到前一位，Rank(次序)要保持连续 */
    for (uint32_t i = (uint32_t)target_index; i < current_count - 1; i++) 
    {
        pdev->host->channels[i].channel_id  = pdev->host->channels[i + 1].channel_id;
        pdev->host->channels[i].sample_time = pdev->host->channels[i + 1].sample_time;
    }

    /**< 先把被挤出来的、硬件上多余的最后一个 Rank 在寄存器中清除（抹除脏数据残留） */
    LL_ADC_REG_SetSequencerRanks(adc_handle, pdev->host->channels[current_count - 1].rank, LL_ADC_CHANNEL_0);

    /**< 双账本计数器必须同步递减 */
    pdev->host->config.channel_num--;
    pdev->host->channel_count--;

    /**<将已经计算并移位好的序列长度直接投喂给库函数 */
    LL_ADC_REG_SetSequencerLength(adc_handle, __HAL_ADC_F4_CH_NUM_TO_REG(pdev->host->config.channel_num));

    /**< 重新刷新所有有效 Rank 的通道映射 */
    for (uint32_t i = 0; i < pdev->host->config.channel_num; i++) 
    {
        LL_ADC_REG_SetSequencerRanks(adc_handle, pdev->host->channels[i].rank, pdev->host->channels[i].channel_id);
        LL_ADC_SetChannelSamplingTime(adc_handle, pdev->host->channels[i].channel_id, pdev->host->channels[i].sample_time);
        }

    if (is_enabled)
        LL_ADC_Enable(adc_handle);

    return MINI_OK;
}

/*===========================================================================================================================================================*/
/*启动停止控制*/
/*===========================================================================================================================================================*/

/**
 * @brief 启动 ADC (Enable + tSTAB 稳定延迟)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_start(hal_adc_device *pdev)
{
    if (!pdev || !pdev->host->adc_handle)
        return MINI_ERR_INVAL;

    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;

    if (LL_ADC_IsEnabled(adc_handle) == 0) 
    {
        LL_ADC_Enable(adc_handle);
        /**< 硬件直投要求：STM32F4 架构在 ADC 启动后需要微小的物理稳定延迟时间 (tSTAB) */
        volatile uint32_t delay = 3 * (SystemCoreClock / 1000000);
        while(delay--);
    }
    return MINI_OK;
}

/**
 * @brief 停止 ADC (LL_ADC_Disable)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_stop(hal_adc_device *pdev)
{
    if (!pdev || !pdev->host->adc_handle)
        return MINI_ERR_INVAL;

    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;

    if (LL_ADC_IsEnabled(adc_handle) != 0)
    {
        LL_ADC_Disable(adc_handle);
    }
    return MINI_OK;
}

/**
 * @brief 启动 ADC DMA 采集 (设长度、Enable stream、联动 ADC DMA)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_dma_start(hal_adc_device *pdev)
{
    if (!pdev || !pdev->host->adc_handle || !pdev->host->dma_cfg.dma_handle || !pdev->host->private_cfg)
        return MINI_ERR_INVAL;

    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
    DMA_TypeDef*  dma        = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
    uint32_t      stream     = pdev->host->dma_cfg.dma_stream;

    /**< 热路径: 静态参数 (方向/地址/通道/优先级) 已在 hal_adc_init 经 LL_DMA_Init 配好;
     *   NORMAL 模式 stream 传完自动停, 重启需重设长度 + 启动 */
    LL_DMA_SetDataLength(dma, stream, DMA_BUFFER_SIZE);
    LL_DMA_EnableStream(dma, stream);

    /**< 联动激活 ADC 外设端的 DMA 传输通道请求触发 */
    LL_ADC_REG_SetDMATransfer(adc_handle, pdev->host->config.dma_mode);

    return hal_adc_start(pdev);
}

/**
 * @brief 启动 ADC DMA+中断采集 (绑定下半部 work、Enable TC IT)
 * @param pdev ADC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_adc_dma_it_start(hal_adc_device *pdev)
 {
     if (!pdev || !pdev->host->adc_handle || !pdev->host->dma_cfg.dma_handle || !pdev->host->private_cfg)
         return MINI_ERR_INVAL;
 
     ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
     DMA_TypeDef*  dma        = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
     uint32_t      stream     = pdev->host->dma_cfg.dma_stream;
     pdev->host->private_cfg->dma_it_enable = true;
 
     /**< 热路径: 静态参数 (方向/地址/通道/优先级) 已在 hal_adc_init 经 LL_DMA_Init 配好;
      *   NORMAL 模式 stream 传完自动停, 重启需重设长度 + 启动 */
     LL_DMA_SetDataLength(dma, stream, DMA_BUFFER_SIZE);
 
     fifo_init(&pdev->host->private_cfg->dma_buffer_handle, pdev->host->private_cfg->dma_data_buf, DMA_BUFFER_SIZE);

     /**< 一次性绑定全局下半部 work (fn/arg/原子位) — 全局变量由 interrupt.c 定义 */
     g_adc_dma_bottom_half_work.fn  = hal_adc_dma_bottom_half_handler;
     g_adc_dma_bottom_half_work.arg = pdev;
     COMPAT_ATOMIC_STORE(&g_adc_dma_bottom_half_work.pending,   false, COMPAT_MO_SEQ_CST);
     COMPAT_ATOMIC_STORE(&g_adc_dma_bottom_half_work.executing, false, COMPAT_MO_SEQ_CST);
     COMPAT_ATOMIC_STORE(&g_adc_dma_bottom_half_work.rerun,     false, COMPAT_MO_SEQ_CST);

     LL_DMA_EnableStream(dma, stream);
     LL_DMA_EnableIT_TC(dma, stream);
     LL_ADC_REG_SetDMATransfer(adc_handle, pdev->host->config.dma_mode);
 
     return hal_adc_start(pdev);
 }

/*===========================================================================================================================================================*/
/*数据读取接口*/
/*==========================================================================================================================================================*/

/**
 * @brief 软件触发单次 ADC 转换并轮询读 DR (仅单通道+SW 触发)
 * @param pdev ADC 设备指针
 * @param channel_num 通道号 (当前实现忽略, 保留参数)
 * @param out_val 输出 12-bit 转换值
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
 int hal_adc_read_value(hal_adc_device *pdev, uint32_t channel_num, uint16_t *out_val)
 {
    COMPAT_IGNORE_RESULT(channel_num);
     if (!pdev || !pdev->host->adc_handle || !out_val)
         return MINI_ERR_INVAL;
 
     ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
 
     /**< 边界拦截：若配置为多通道扫描或非软件独立触发，直接返回不受理错误码 */
    if (pdev->host->channel_count > 1 || pdev->host->config.trigger_src != (int32_t)pdev->host->config.sw_trigger)
         return MINI_ERR_NOTSUPP;

     /**< 确保 ADC 已使能 */
     if (LL_ADC_IsEnabled(adc_handle) == 0)
         return MINI_ERR_NODEV;
 
     /**< 刷新清除残留的旧规则组转换结束标志 */
     LL_ADC_ClearFlag_EOCS(adc_handle);

     /**< 软件启动转换路径 */
     LL_ADC_REG_StartConversionSWStart(adc_handle);
 
     /**< 轮询等待 EOCS 标志置位 */
     uint32_t timeout = 0x00FFFFF;
     while (LL_ADC_IsActiveFlag_EOCS(adc_handle) == 0)
     {
         if (--timeout == 0) 
            return MINI_ERR_AGAIN;
     }
 
     /**< 读出并输出数据（读取 DR 寄存器会自动清除 EOCS 标志） */
     *out_val = LL_ADC_REG_ReadConversionData12(adc_handle);
 
     return MINI_OK;
 }
 
/**
 * @brief 查询 ADC 转换完成标志 (EOCS)
 * @param pdev ADC 设备指针
 * @param out_status 输出 1=完成, 0=未完成
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_poll_for_conversion(hal_adc_device *pdev, uint32_t *out_status)
{
    if (!pdev || !pdev->host->adc_handle || !out_status)
        return MINI_ERR_INVAL;
 
    ADC_TypeDef* adc_handle = (ADC_TypeDef*)pdev->host->adc_handle;
 
    /**< 直接透出当前 EOCS 标志状态 (1 为完成，0 为未完成) */
    *out_status = LL_ADC_IsActiveFlag_EOCS(adc_handle);
 
    return MINI_OK;
}

/**
 * @brief 获取当前配置的 ADC 通道数量
 * @param pdev ADC 设备指针
 * @param count 输出通道数
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_count(hal_adc_device *pdev, uint32_t*count)
{
    if (!pdev || !count)
        return MINI_ERR_INVAL;

    *count = (int)pdev->host->channel_count;
    return MINI_OK;
}

/**
 * @brief 按索引获取 ADC 通道 ID
 * @param pdev ADC 设备指针
 * @param index 通道索引 (0..count-1)
 * @param channel_id 输出通道 ID
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_id(hal_adc_device *pdev, int index, uint32_t *channel_id)
{
    if (!pdev || !channel_id || index < 0 || (uint32_t)index >= pdev->host->channel_count)
        return MINI_ERR_INVAL;

    *channel_id = pdev->host->channels[index].channel_id;
    return MINI_OK;
}

/**
 * @brief 按索引获取 ADC 通道采样时间
 * @param pdev ADC 设备指针
 * @param index 通道索引
 * @param sample_time 输出采样时间
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_sample_time(hal_adc_device *pdev, int index, uint32_t *sample_time)
{
    if (!pdev || !sample_time || index < 0 || (uint32_t)index >= pdev->host->channel_count)
        return MINI_ERR_INVAL;

    *sample_time = pdev->host->channels[index].sample_time;
    return MINI_OK;
}

/*===========================================================================================================================================================*/
/*DMA 快速路径接口*/
/*===========================================================================================================================================================*/

/**
 * @brief 从 ADC DR 寄存器直接读取 DMA 采样值 (热路径)
 * @param pdev ADC 设备指针
 * @param out_val 输出采样值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_adc_dma_read_value(hal_adc_device *pdev, uint16_t *out_val)
{
    /**<热路径优化:直接读取 DR 寄存器 */
#ifdef HARD_PATH_STRICT_CHECK
    if (!pdev || !out_val || pdev->host->dev_index >= DTS_HAL_ADC_INSTANCE_MAX) return MINI_ERR_INVAL;
#endif

    *out_val = g_adc_fast_tbl[pdev->host->dev_index]->DR;
    
    return MINI_OK;
}

/**
 * @brief 从 DMA 中断 fifo 读一个采样值
 * @param pdev ADC 设备指针
 * @param out_val 输出采样值
 * @return 成功返回 MINI_OK, fifo 空返回 MINI_ERR_AGAIN
 */
int hal_adc_dma_it_read_value(hal_adc_device *pdev, uint16_t *out_val)
{
    if (!pdev || !pdev->host->adc_handle || !pdev->host->private_cfg || !out_val)
        return MINI_ERR_INVAL;

    fifo_data_type tmp;
    if (fifo_read_data(&pdev->host->private_cfg->dma_buffer_handle, &tmp))
    {
        *out_val = (uint16_t)tmp;
        return MINI_OK;
    }

    /**< 读空代表当前数据未就绪，必须返回 MINI_ERR_AGAIN 提示上层稍后再试 */
    return MINI_ERR_AGAIN;
}

/**
 * @brief ADC 虚拟中断上半部回调 (ISR 内执行)
 * @param arg ADC 设备指针 (hal_adc_device*)
 * @param irq_num 虚拟中断号
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部 fifo 拷贝
 */
int hal_virtual_adc_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(irq_num);
    hal_adc_device* pdev = (hal_adc_device*)arg;

    if (!pdev || !pdev->host || !pdev->host->private_cfg)
        return MINI_ERR_INVAL;
    return MINI_IRQ_ENTRY_BOTTOM;  /**< 需要 submit 下半部执行 fifo 拷贝 */
}

/**
 * @brief ADC DMA 下半部处理 (raw buf → fifo)
 * @param arg ADC 设备指针 (hal_adc_device*)
 */
void hal_adc_dma_bottom_half_handler(void* arg)
{
    hal_adc_device* pdev = (hal_adc_device*)arg;
    if (!pdev || !pdev->host || !pdev->host->private_cfg)
        return;

    struct hal_adc_private_cfg* cfg = pdev->host->private_cfg;
    uint16_t written = 0;
    for (uint16_t i = 0; i < DMA_BUFFER_SIZE; i++)
    {
        if (!fifo_write_data(&cfg->dma_buffer_handle, (fifo_data_type)cfg->dma_raw_data_buf[i]))
            break;
        written++;
    }
    if (written == DMA_BUFFER_SIZE)
        SYS_LOGE("DMA_ADC", "dma_it_trans_err_fill_up_buff");
}
