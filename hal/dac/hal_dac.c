/**
 * @license: SPDX-License-Identifier: Apache-2.0 
 * @file hal_dac.c
 * @brief DAC HAL 层 — 硬件抽象接口,硬件直投层
 * @note 所有接口设计为平台无关，由具体芯片平台(如 STM32, ESP32, CH307)进行底层硬实现。
 * @note 由于DAC是快速热路径外设所以DAC的初始化与配置应该尽量在硬件直投层完成
 * @note 文件约定：返回值不允许void，必须使用int，并且错误码使用 status.h 中的 MINI_ERR_* 体系 
 * @note 接收的参数必须为指针，并且必须为合法的指针，不能为空指针
 * @note 禁止使用enum,enum的问题dts已经解决没必要在hal层重复定义去映射enum不直观而且麻烦还容易出错
*/
#include "hal_dac.h"
#include "stm32f4xx_ll_dac.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_dma.h"
#include "interrupt.h"

/** DAC 下半部工作项 (fn/arg 由 VFS 层绑定), 供 interrupt_virtual_register 注册 */
struct bottom_half_work g_dac_bottom_half_work;


/**
 * @brief 清除 DMA stream 对应的 TC 标志 (STM32F4 LL 库按 stream 编号分布)
 * @param dma DMA 控制器基址
 * @param stream DMA 流编号 (0..7)
 */
static void hal_dac_dma_clear_tc(DMA_TypeDef* dma, uint32_t stream)
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
 * @brief 配置 GPIO 引脚
 * @param gpio GPIO 配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
MINI_STATIC_INLINE int hal_dac_config_af_pin(hal_dac_gpio_config* gpio)
{
    if (!gpio)
        return MINI_ERR_INVAL;
    
    GPIO_TypeDef* port = (GPIO_TypeDef*)gpio->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    GPIO_InitStruct.Pin        = gpio->pin;
    GPIO_InitStruct.Mode       = gpio->mode;
    GPIO_InitStruct.Pull       = gpio->pull;
    GPIO_InitStruct.Alternate  = gpio->af;
    GPIO_InitStruct.Speed      = gpio->speed;
    GPIO_InitStruct.OutputType = gpio->output_type;
    
    LL_AHB1_GRP1_EnableClock(gpio->clk_bus);
    if (LL_GPIO_Init(port, &GPIO_InitStruct) != SUCCESS)
        return MINI_ERR_INVAL;

    return MINI_OK;
}

/**
 * @brief 初始化 DAC 设备对象 (绑定 host 与平台私有配置)
 * @param pdev DAC 设备指针
 * @param host_cfg host 配置指针
 * @param unique_cfg 平台私有配置指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_device_init(hal_dac_device* pdev, hal_dac_host_config* host_cfg, hal_dac_platform_unique_config* unique_cfg)
{
    if (!pdev || !host_cfg || !unique_cfg)
        return MINI_ERR_INVAL;

    pdev->host   = host_cfg;
    pdev->unique = unique_cfg;
    return MINI_OK;
}

/**
 * @brief 关闭 DAC 设备对象 (清空 host/unique 引用)
 * @param dev DAC 设备指针
 * @return 成功返回 MINI_OK
 */
int hal_dac_close(hal_dac_device* dev)
{
    if (!dev)
        return MINI_OK;

    dev->unique = NULL;
    dev->host   = NULL;
    return MINI_OK;
}

/**
 * @brief 设置 DAC 12-bit 右对齐输出值
 * @param pdev DAC 设备指针
 * @param value 输出值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_set_value(hal_dac_device* pdev, uint32_t value)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;
        
    LL_DAC_ConvertData12RightAligned((DAC_TypeDef*)pdev->host->dac_handle, pdev->host->config.channel, value);
    return MINI_OK;
}

/**
 * @brief 读取 DMA 剩余传输计数 (NDTR)
 * @param pdev DAC 设备指针
 * @param remaining 输出剩余采样数
 * @return 成功返回 MINI_OK, 非 DMA 模式返回 MINI_ERR_INVAL
 */
int hal_dac_get_dma_progress(hal_dac_device* pdev, uint32_t* remaining)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle || !remaining)
        return MINI_ERR_INVAL;
 
    if (!pdev->host->config.dma_enable) {
        *remaining = 0;
        return MINI_ERR_INVAL; // 非 DMA 模式无意义
    }
 
    DMA_TypeDef* dma = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
    uint32_t stream = pdev->host->dma_cfg.dma_stream;
     
    // 读取 DMA 的 NDTR 寄存器，获取剩余传输量
    *remaining = LL_DMA_GetDataLength(dma, stream);
    return MINI_OK;
}

/**
 * @brief 向 DMA 环形缓冲写入采样 (双缓冲策略, 落后播放指针半圈)
 * @param pdev DAC 设备指针
 * @param data 采样数据源
 * @param len 采样点数
 * @return 成功返回写入长度, 失败返回 MINI_ERR_INVAL
 */
 int hal_dac_write_dma_buffer(hal_dac_device* pdev, const uint16_t* data, uint32_t len)
 {
     if (!pdev || !pdev->host || !pdev->host->dma_cfg.dma_fifo || !data || len == 0)
         return MINI_ERR_INVAL;
 
     uint32_t current_pos = 0;
     // 1. 获取当前 DMA 读到了哪里
     if (hal_dac_get_dma_progress(pdev, &current_pos) != MINI_OK)
         return MINI_ERR_INVAL;
 
     uint16_t* buf  = (uint16_t*)pdev->host->dma_cfg.dma_fifo->buf;
     uint32_t  size = pdev->host->dma_cfg.dma_fifo->size;
 
     // 简单策略：总是从当前播放位置之后的安全区域开始覆盖 (双缓冲思想)
     // 注意：这里是最基础的演示，实际音频处理通常采用双缓冲 (前半段/后半段) 
     // 或在中断 (TC/HT) 中专门填充。这里提供一个直接按偏移填入的方法。
     
     uint32_t write_pos = current_pos + (size / 2); // 落后当前播放指针半圈
     if (write_pos >= size) 
         write_pos -= size;
 
     for (uint32_t i = 0; i < len; i++) 
     {
         buf[write_pos] = data[i];
         write_pos++;
         if (write_pos >= size)
             write_pos = 0; // 环形回卷
     }
 
     return (int)len;
 }
 
/**
 * @brief 读取 DAC 当前输出寄存器值
 * @param pdev DAC 设备指针
 * @param value 输出值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_get_value(hal_dac_device* pdev, uint32_t* value)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle || !value)
        return MINI_ERR_INVAL;
        
    *value = LL_DAC_RetrieveOutputData((DAC_TypeDef*)pdev->host->dac_handle, pdev->host->config.channel);
    return MINI_OK;
}

/**
 * @brief 使能 DAC 输出（可选 DMA 请求与 underrun 中断）
 * @param pdev DAC 设备
 * @param dma_req 非 0 时打开 DAC DMA 请求
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
static int hal_dac_enable_output(hal_dac_device* pdev, int dma_req)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    DAC_TypeDef* dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    uint32_t     channel = pdev->host->config.channel;

    if (pdev->host->config.it_enable) 
    {
        if (channel == LL_DAC_CHANNEL_1)
            LL_DAC_EnableIT_DMAUDR1(dac);
        else if (channel == LL_DAC_CHANNEL_2)
            LL_DAC_EnableIT_DMAUDR2(dac);
    }

    if (dma_req)
        LL_DAC_EnableDMAReq(dac, channel);

    LL_DAC_Enable(dac, channel);
    return MINI_OK;
}

/**
 * @brief 配置并启动 DMA（可选 TC 中断）
 * @param pdev DAC 设备
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
static int hal_dac_start_dma_or_it(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle || !pdev->host->dma_cfg.dma_handle
        || !pdev->host->dma_cfg.dma_fifo || !pdev->host->dma_cfg.dma_fifo->buf)
        return MINI_ERR_INVAL;

    DAC_TypeDef*       dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    DMA_TypeDef*       dma     = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
    uint32_t           stream  = pdev->host->dma_cfg.dma_stream;
    uint32_t           nb_data = pdev->host->dma_cfg.dma_buffer_size;
    uint32_t           dma_reg = pdev->host->config.dma_data_align;
    LL_DMA_InitTypeDef dma_init_handle = {0};

    if (!nb_data)
        nb_data = pdev->host->dma_cfg.dma_fifo->size;
    if (!nb_data)
        return MINI_ERR_INVAL;

    /** STM32F4 要求配置前 stream disable */
    LL_DMA_DisableStream(dma, stream);

    dma_init_handle.PeriphOrM2MSrcAddress  = LL_DAC_DMA_GetRegAddr(dac, pdev->host->config.channel, dma_reg);
    dma_init_handle.MemoryOrM2MDstAddress  = (uint32_t)pdev->host->dma_cfg.dma_fifo->buf;
    dma_init_handle.Direction              = pdev->host->dma_cfg.dma_direction;
    dma_init_handle.Mode                   = pdev->host->dma_cfg.dma_mode;
    dma_init_handle.PeriphOrM2MSrcIncMode  = pdev->host->dma_cfg.dma_periph_inc;
    dma_init_handle.MemoryOrM2MDstIncMode  = pdev->host->dma_cfg.dma_mem_inc;
    dma_init_handle.PeriphOrM2MSrcDataSize = pdev->host->dma_cfg.dma_periph_data_size ? pdev->host->dma_cfg.dma_periph_data_size : pdev->host->dma_cfg.dma_data_size;
    dma_init_handle.MemoryOrM2MDstDataSize = pdev->host->dma_cfg.dma_data_size;
    dma_init_handle.NbData                 = nb_data;
    dma_init_handle.Channel                = pdev->host->dma_cfg.dma_channel;
    dma_init_handle.Priority               = pdev->host->dma_cfg.dma_priority;
    dma_init_handle.FIFOMode               = pdev->host->dma_cfg.dma_fifo_is_enable;
    dma_init_handle.FIFOThreshold          = pdev->host->dma_cfg.dma_fifo_threshold ? pdev->host->dma_cfg.dma_fifo_threshold : pdev->host->dma_cfg.dma_fifo_mode;
    dma_init_handle.MemBurst               = pdev->host->dma_cfg.dma_mem_burst;
    dma_init_handle.PeriphBurst            = pdev->host->dma_cfg.dma_periph_burst;

    LL_DMA_Init(dma, stream, &dma_init_handle);

    if (pdev->host->config.it_enable)
        LL_DMA_EnableIT_TC(dma, stream);

    LL_DMA_EnableStream(dma, stream);
    return MINI_OK;
}

/**
 * @brief 非 DMA 模式启动 DAC
 * @param pdev DAC 设备
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
static int hal_dac_base_or_it_start(hal_dac_device* pdev)
{
    return hal_dac_enable_output(pdev, 0);
}

/**
 * @brief 初始化 DAC 硬件 (时钟/GPIO/LL_DAC_Init)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_init(hal_dac_device* pdev)
{
    int ret;

    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    /** 使能 DAC 外设时钟 */
    LL_APB1_GRP1_EnableClock(pdev->host->config.dac_clk_periph);

    /** 先配置 DAC 输出引脚 (ANALOG 模式), 否则 DAC 电压无法到达物理引脚 */
    ret = hal_dac_config_af_pin(&pdev->host->gpio_cfg);
    if (ret != MINI_OK)
        return ret;

    LL_DAC_InitTypeDef dac_init_handle = {0};
    dac_init_handle.OutputBuffer                = pdev->host->config.output_buf;
    dac_init_handle.WaveAutoGenerationConfig    = pdev->host->config.wave_auto_generation_config;
    dac_init_handle.WaveAutoGeneration          = pdev->host->config.wave_auto_generation_mode;
    dac_init_handle.TriggerSource               = pdev->host->config.trigger_source;

    LL_DAC_Init((DAC_TypeDef*)pdev->host->dac_handle, pdev->host->config.channel, &dac_init_handle);
    return MINI_OK;
}

/**
 * @brief 启动 DAC 输出 (DMA 或 base 路径, 软件触发时补一次 SW 转换)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_start(hal_dac_device* pdev)
{
    int ret;

    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    /* 重新启动前，必须显式开启触发源 */
    LL_DAC_EnableTrigger((DAC_TypeDef*)pdev->host->dac_handle, pdev->host->config.channel);

    if (pdev->host->config.dma_enable) 
    {
        ret = hal_dac_start_dma_or_it(pdev);
        if (ret != MINI_OK)
            return ret;
        ret = hal_dac_enable_output(pdev, 1);
    } else 
        ret = hal_dac_base_or_it_start(pdev);
    
    /* 如果是软件触发，需手动产生一次触发信号 */
    if (pdev->host->config.trigger_source == pdev->host->config.dac_sw_trigger)
        LL_DAC_TrigSWConversion((DAC_TypeDef*)pdev->host->dac_handle, pdev->host->config.channel);

    return ret;
}

/**
 * @brief 暂停 DAC 通道输出（关闭触发与通道，不关闭 DMA Stream）
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
static int hal_dac_channel_pause(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    DAC_TypeDef* dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    uint32_t     channel = pdev->host->config.channel;

    LL_DAC_DisableTrigger(dac, channel);
    LL_DAC_Disable(dac, channel);

    return MINI_OK;
}

/**
 * @brief 暂停 DAC DMA 模式输出 (关触发与通道, 不关 DMA stream)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 非 DMA 模式返回 MINI_ERR_AGAIN
 */
int hal_dac_dma_pause(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    if (!pdev->host->config.dma_enable)
        return MINI_ERR_AGAIN;

    return hal_dac_channel_pause(pdev);
}

/**
 * @brief 暂停 DAC 非 DMA 模式输出
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, DMA 模式返回 MINI_ERR_AGAIN
 */
int hal_dac_base_pause(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    if (pdev->host->config.dma_enable)
        return MINI_ERR_AGAIN;

    return hal_dac_channel_pause(pdev);
}

/**
 * @brief 暂停 DAC 输出 (按 dma_enable 分派 dma/base 路径)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_dac_pause(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    if (pdev->host->config.dma_enable)
        return hal_dac_dma_pause(pdev);

    return hal_dac_base_pause(pdev);
}

/**
 * @brief 恢复 DAC 输出 (Enable 通道/触发, SW 触发时补一次转换)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_resume(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    DAC_TypeDef* dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    uint32_t     channel = pdev->host->config.channel;

    /**< 使能 DAC 通道 */
    LL_DAC_Enable(dac, channel);

    /**< 使能触发源 */
    LL_DAC_EnableTrigger(dac, channel);

    /**< 若为软件触发, 恢复时手动产生一次 */
    if (pdev->host->config.trigger_source == pdev->host->config.dac_sw_trigger)
    {
        LL_DAC_TrigSWConversion(dac, channel);
    }

    return MINI_OK;
}

/**
 * @brief 停止 DAC DMA 输出 (关通道/触发/DMA/中断)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 非 DMA 模式返回 MINI_ERR_AGAIN
 */
int hal_dac_stop_dma(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_ERR_INVAL;

    DAC_TypeDef* dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    uint32_t     channel = pdev->host->config.channel;

    /* 1. 先关闭 DAC 输出和触发，切断 DMA 的需求源头 */
    LL_DAC_Disable(dac, channel);
    LL_DAC_DisableTrigger(dac, channel);
    LL_DAC_DisableDMAReq(dac, channel);

    /* 2. 处理中断标志位清理 (防止重启后因为残留标志位误触发中断锁死) */
    if (pdev->host->config.it_enable) 
    {
        if (channel == LL_DAC_CHANNEL_1) {
            LL_DAC_ClearFlag_DMAUDR1(dac);
            LL_DAC_DisableIT_DMAUDR1(dac);
        }
        else if (channel == LL_DAC_CHANNEL_2) {
            LL_DAC_ClearFlag_DMAUDR2(dac);
            LL_DAC_DisableIT_DMAUDR2(dac);
        }
    }

    /**< 关闭 DMA Stream */
    if (pdev->host->config.dma_enable) 
    {
        DMA_TypeDef* dma = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
        if (!dma)
            return MINI_ERR_INVAL;

        /**< 关闭 DMA 传输完成中断 */
        if (pdev->host->config.it_enable)
            LL_DMA_DisableIT_TC(dma, pdev->host->dma_cfg.dma_stream);

        /** < 关闭 Stream> */
        LL_DMA_DisableStream(dma, pdev->host->dma_cfg.dma_stream);
    }
    else
        return MINI_ERR_AGAIN;

    return MINI_OK;
}

/**
 * @brief 停止 DAC 非 DMA 输出 (关通道/触发, 清 underrun IT)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_dac_base_stop(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host || !pdev->host->dac_handle)
    return MINI_ERR_INVAL;

    DAC_TypeDef* dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    uint32_t     channel = pdev->host->config.channel;

    /* 1. 先关闭 DAC 输出和触发，切断 DMA 的需求源头 */
    LL_DAC_Disable(dac, channel);
    LL_DAC_DisableTrigger(dac, channel);
    LL_DAC_DisableDMAReq(dac, channel);

    /* 2. 处理中断标志位清理 (防止重启后因为残留标志位误触发中断锁死) */
    if (pdev->host->config.it_enable) 
    {
        if (channel == LL_DAC_CHANNEL_1) {
            LL_DAC_ClearFlag_DMAUDR1(dac);
            LL_DAC_DisableIT_DMAUDR1(dac);
        }
        else if (channel == LL_DAC_CHANNEL_2) {
            LL_DAC_ClearFlag_DMAUDR2(dac);
            LL_DAC_DisableIT_DMAUDR2(dac);
        }
    }
    return  MINI_OK;
}
/**
 * @brief 强制停止 DAC (按 dma_enable 分派 stop_dma/base_stop)
 * @param pdev DAC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 VFS_ERR_*
 */
int hal_dac_force_stop(hal_dac_device* pdev)
{
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    if (pdev->host->config.dma_enable)
        return hal_dac_stop_dma(pdev);
    else
        return hal_dac_base_stop(pdev); 
}

/* =========================================================================================================================================================== */
/* ISR 虚拟中断回调                                                                                                                                              */
/* =========================================================================================================================================================== */

/**
 * @brief DAC 虚拟中断上半部回调 (清除 DMA TC / underrun 标志)
 * @param arg DAC 设备指针 (hal_dac_device*)
 * @param irq_num 虚拟中断号 (当前忽略)
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部; MINI_IRQ_ENTRY_NOBOTTOM 不需要
 */
int hal_virtual_dac_irq_callback(void* arg, uint16_t irq_num)
{
    MINI_IGNORE_RESULT(irq_num);
    hal_dac_device* pdev = (hal_dac_device*)arg;

    if (!pdev || !pdev->host || !pdev->host->dac_handle)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    DAC_TypeDef*       dac     = (DAC_TypeDef*)pdev->host->dac_handle;
    DMA_TypeDef*       dma     = (DMA_TypeDef*)pdev->host->dma_cfg.dma_handle;
    uint32_t           stream  = pdev->host->dma_cfg.dma_stream;
    uint32_t           channel = pdev->host->config.channel;

    /** 清除 DMA TC 标志 */
    if (dma && pdev->host->config.dma_enable)
        hal_dac_dma_clear_tc(dma, stream);

    /** 清除 DAC underrun 标志 */
    if (pdev->host->config.it_enable)
    {
        if (channel == LL_DAC_CHANNEL_1)
            LL_DAC_ClearFlag_DMAUDR1(dac);
        else if (channel == LL_DAC_CHANNEL_2)
            LL_DAC_ClearFlag_DMAUDR2(dac);
    }

    return MINI_IRQ_ENTRY_BOTTOM;
}
