/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_adc_ch32v30x.c
 *@brief CH32V307 ADC HAL 强符号实现 (覆盖 mini_tree/hal/adc/hal_adc.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头。
 *   直投约定 (对齐 hal_adc.h):
 *   - adc_handle = ADCx_BASE, config.adc_clk_bus = RCC_APB2Periph_ADCx;
 *   - 引脚缺省模拟输入 (GPIO_Mode_AIN);
 *   - ADCCLK 分频从 {2,4,6,8} 中选满足 <=14MHz 的最小分频 (144MHz 下退到
 *     Div8=18MHz, 与 WCH EVT 一致);
 *   - 规则组软件触发 (ADC_ExternalTrigConv_None 缺省), 扫描/连续/对齐直投;
 *   - read_value 为单通道轮询读: 现配通道 (rank=1) + 软启动 + 等 EOC + 读 RDATAR;
 *   - DMA 路径: ADC1 走 DMA1_CH1 循环搬运到内部环形缓冲 (连续转换),
 *     read_value 取最新样本; ADC2 无 DMA1 映射返回 MINI_ERR_NOTSUPP;
 *   - 差分/衰减字段本芯片无对应硬件, 忽略。
 */

#include "ch32v30x_hal_common.h"
#include "hal_adc.h"

/* =============================================================================
 * 内部辅助 (时钟分频选择 / 通道采样时间查表)
 * ============================================================================= */

/**
 * @brief 选择 ADCCLK 分频 (从 {2,4,6,8} 取满足 <=14MHz 的最小分频, 否则兜底 Div8)
 * @param[in] adc_base ADCx_BASE (用于折算所在 APB2 总线频率)
 * @return RCC_PCLK2_Div2/4/6/8 宏值
 */
static uint32_t s_adc_clk_div(uintptr_t adc_base)
{
    uint32_t pclk2 = ch32_apb_freq(adc_base);
    if ((pclk2 / 2U) <= 14000000U)
        return RCC_PCLK2_Div2;
    if ((pclk2 / 4U) <= 14000000U)
        return RCC_PCLK2_Div4;
    if ((pclk2 / 6U) <= 14000000U)
        return RCC_PCLK2_Div6;
    return RCC_PCLK2_Div8;
}

/**
 * @brief 在 host->channels[] 中查通道采样时间 (未命中缺省最长采样 239.5 周期)
 * @param[in] host ADC 主机配置 (出参, 仅读通道表)
 * @param[in] channel_id 目标硬件通道号
 * @return ADC_SampleTime_* 宏值
 */
static uint8_t s_adc_sample_time(const hal_adc_host_config* host, uint32_t channel_id)
{
    if (host->channels != NULL)
    {
        for (uint32_t i = 0; i < host->channel_count; i++)
        {
            if (host->channels[i].channel_id == channel_id)
            {
                if (host->channels[i].sample_time != 0U)
                    return (uint8_t)host->channels[i].sample_time;
                break;
            }
        }
    }
    return ADC_SampleTime_239Cycles5;
}

/**
 * @brief 复位校准并等待完成 (带空转预算保护)
 * @param[in] adc ADC 外设寄存器指针 (出参, 写校准控制位)
 * @return 成功返回 MINI_OK, 校准超时返回 MINI_ERR_TIMEOUT
 */
static int s_adc_calibrate(ADC_TypeDef* adc)
{
    uint32_t budget = ch32_poll_budget(10U);

    ADC_ResetCalibration(adc);
    while (ADC_GetResetCalibrationStatus(adc) != RESET)
    {
        if (budget == 0U)
            return MINI_ERR_TIMEOUT;
        budget--;
    }
    ADC_StartCalibration(adc);
    while (ADC_GetCalibrationStatus(adc) != RESET)
    {
        if (budget == 0U)
            return MINI_ERR_TIMEOUT;
        budget--;
    }
    return MINI_OK;
}

/* =============================================================================
 * DMA 环形缓冲 (循环搬运: 外设→内部缓冲, 接口不携带缓冲区)
 * ============================================================================= */

#define CH32_ADC_DMA_BUF_LEN 16U

typedef struct
{
    uintptr_t adc_base; /**< ADCx_BASE (匹配键) */
    DMA_Channel_TypeDef* ch; /**< DMA 通道 (启动后缓存) */
    uint16_t* buf; /**< 环形缓冲 */
    uint32_t len; /**< 有效单元数 (= 扫描序列长度) */
    bool active; /**< DMA 已启动标志 */
} ch32_adc_dma_state_t;

static uint16_t s_adc1_dma_buf[CH32_ADC_DMA_BUF_LEN];

/* ADC2 无 DMA1 映射, 仅登记 ADC1 */
static ch32_adc_dma_state_t s_adc_dma_state[] = {
    {ADC1_BASE, NULL, s_adc1_dma_buf, 0U, false},
};

/**
 * @brief 启动 DMA1_CH1 循环搬运 (外设 RDATAR→环形缓冲, 可选传输完成中断标志)
 * @param[in] periph_addr ADC 数据寄存器地址 (&ADCx->RDATAR)
 * @param[in] mem_addr 环形缓冲区地址 (半字对齐)
 * @param[in] units 环形缓冲区单元数 (1..65535)
 * @param[in] tc_it 是否使能传输完成中断标志 (NVIC 使能由板级中断子系统负责)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
static int s_adc_dma_start_circ(uint32_t periph_addr, uint32_t mem_addr, uint32_t units, bool tc_it)
{
    if ((units == 0U) || (units > 0xFFFFU))
        return MINI_ERR_INVAL;
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    DMA_Cmd(DMA1_Channel1, DISABLE);

    DMA_InitTypeDef di;
    di.DMA_PeripheralBaseAddr = periph_addr;
    di.DMA_MemoryBaseAddr = mem_addr;
    di.DMA_DIR = DMA_DIR_PeripheralSRC;
    di.DMA_BufferSize = units;
    di.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    di.DMA_MemoryInc = DMA_MemoryInc_Enable;
    di.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    di.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    di.DMA_Mode = DMA_Mode_Circular;
    di.DMA_Priority = DMA_Priority_High;
    di.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &di);

    DMA_ClearFlag(DMA1_FLAG_TC1);
    DMA_ClearFlag(DMA1_FLAG_TE1);
    DMA_ITConfig(DMA1_Channel1, DMA_IT_TC, tc_it ? ENABLE : DISABLE);
    DMA_Cmd(DMA1_Channel1, ENABLE);
    return MINI_OK;
}

/**
 * @brief 按 ADC 基址查 DMA 状态槽 (未登记返回 NULL)
 * @param[in] adc_base ADCx_BASE
 * @return 状态槽指针, 未登记返回 NULL
 */
static ch32_adc_dma_state_t* s_adc_dma_lookup(uintptr_t adc_base)
{
    for (size_t i = 0; i < (sizeof(s_adc_dma_state) / sizeof(s_adc_dma_state[0])); i++)
        if (s_adc_dma_state[i].adc_base == adc_base)
            return &s_adc_dma_state[i];
    return NULL;
}

/**
 * @brief 启动循环 DMA 采样 (装配规则组序列 + 强制连续转换 + 软启动)
 * @param[in] pdev ADC 设备指针 (已绑定 host 且已 hal_adc_init)
 * @param[in] tc_it 是否使能 DMA 传输完成中断标志 (NVIC 由板级中断子系统负责)
 * @return 成功返回 MINI_OK, 无 DMA 映射/通道表缺失返回 MINI_ERR_NOTSUPP,
 *         序列超过缓冲容量返回 MINI_ERR_NOMEM, 参数无效返回 MINI_ERR_INVAL
 */
static int s_adc_dma_start(hal_adc_device* pdev, bool tc_it)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    hal_adc_host_config* host = pdev->host;
    ADC_TypeDef* adc = (ADC_TypeDef*)host->adc_handle;
    ch32_adc_dma_state_t* st = s_adc_dma_lookup(host->adc_handle);
    if ((adc == NULL) || (st == NULL))
        return MINI_ERR_NOTSUPP; /* 仅 ADC1 登记 (ADC2 无 DMA1 映射) */

    /* 扫描序列: 优先取通道表, 长度以 config.channel_num 兼容 */
    uint32_t n = (host->config.channel_num > 0) ? host->config.channel_num : host->channel_count;
    if ((n == 0U) || (host->channels == NULL))
        return MINI_ERR_INVAL;
    if (n > CH32_ADC_DMA_BUF_LEN)
        return MINI_ERR_NOMEM;

    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t ch_id = host->channels[i].channel_id;
        ADC_RegularChannelConfig(adc, ch_id, (uint8_t)(i + 1U), s_adc_sample_time(host, ch_id));
    }
    adc->RSQR1 = (adc->RSQR1 & ~((uint32_t)0xFU << 20)) | ((n - 1U) << 20);

    /* 强制连续转换 (DMA 循环搬运需要), 软启动触发 */
    adc->CTLR2 |= (uint32_t)(1U << 1); /* CONT */

    ADC_DMACmd(adc, ENABLE);
    int ret = s_adc_dma_start_circ((uint32_t)&adc->RDATAR, (uint32_t)st->buf, n, tc_it);
    if (ret != MINI_OK)
    {
        ADC_DMACmd(adc, DISABLE);
        return ret;
    }
    ADC_SoftwareStartConvCmd(adc, ENABLE);

    st->ch = DMA1_Channel1;
    st->len = n;
    st->active = true;
    return MINI_OK;
}

/**
 * @brief 从环形缓冲取最新样本 (由 DMA 剩余计数反推写入位置)
 * @param[in] pdev ADC 设备指针 (已调用过 dma_start/dma_it_start)
 * @param[out] out_val 采样值输出指针
 * @return 成功返回 MINI_OK, 未启动 DMA 返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
static int s_adc_dma_read(hal_adc_device* pdev, uint16_t* out_val)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (out_val == NULL))
        return MINI_ERR_INVAL;
    ch32_adc_dma_state_t* st = s_adc_dma_lookup(pdev->host->adc_handle);
    if ((st == NULL) || !st->active)
        return MINI_ERR_NOTSUPP;
    uint32_t rem = (uint32_t)DMA_GetCurrDataCounter(st->ch);
    uint32_t idx = ((st->len - rem) % st->len);
    *out_val = st->buf[idx];
    return MINI_OK;
}

/* =============================================================================
 * 上层接口 (hal_adc.h)
 * ============================================================================= */

/**
 * @brief 绑定 ADC 设备与主机/平台配置 (不碰硬件)
 * @param[in] pdev ADC 设备指针 (出参)
 * @param[in] unique_cfg 平台唯一配置 (出参, private_cfg 写入 ADCx_BASE)
 * @param[in] host ADC 主机配置 (出参, 仅保存指针)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_adc_device_init(hal_adc_device* pdev, hal_adc_platform_unique_config* unique_cfg,
                        hal_adc_host_config* host)
{
    if ((pdev == NULL) || (host == NULL))
        return MINI_ERR_INVAL;
    pdev->host = host;
    pdev->unique = unique_cfg;
    if (unique_cfg != NULL)
        unique_cfg->private_cfg = host->adc_handle;
    return MINI_OK;
}

/**
 * @brief 释放 ADC 设备运行时资源 (仅解绑定, 不关硬件)
 * @param[in] pdev ADC 设备指针 (出参)
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_adc_device_deinit(hal_adc_device* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    pdev->host = NULL;
    pdev->unique = NULL;
    return MINI_OK;
}

/**
 * @brief 初始化 ADC 外设 (时钟 + 模拟引脚 + ADCCLK 分频 + ADC_Init + 校准)
 * @note  缺省: 独立模式 / 右对齐 / 软件触发; 扫描/连续/触发源按 config 直投;
 *        internal_ch_enable 非 0 时开启内部温度传感器/基准通道供电。
 * @param[in] pdev ADC 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL, 校准超时返回 MINI_ERR_TIMEOUT
 */
int hal_adc_init(hal_adc_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    hal_adc_host_config* host = pdev->host;
    ADC_TypeDef* adc = (ADC_TypeDef*)host->adc_handle;
    if (adc == NULL)
        return MINI_ERR_INVAL;

    int ret = ch32_rcc_enable(host->adc_handle, (uint32_t)host->config.adc_clk_bus);
    if (ret != MINI_OK)
        return ret;

    /* 模拟输入引脚 (mode 缺省 AIN) */
    if (host->gpio_cfg.port != 0U)
    {
        ret = ch32_gpio_port_clk(host->gpio_cfg.port, host->gpio_cfg.clk_bus);
        if (ret != MINI_OK)
            return ret;
        uint32_t mode = (host->gpio_cfg.mode != 0U) ? host->gpio_cfg.mode : (uint32_t)GPIO_Mode_AIN;
        ret = ch32_gpio_cfg_pin(host->gpio_cfg.port, ch32_pin_to_mask(host->gpio_cfg.pin), mode,
                                host->gpio_cfg.speed, host->gpio_cfg.pull);
        if (ret != MINI_OK)
            return ret;
    }

    RCC_ADCCLKConfig(s_adc_clk_div(host->adc_handle));

    ADC_InitTypeDef ai;
    ai.ADC_Mode = (host->multi_cfg != NULL && host->multi_cfg->multimode != 0U) ?
                      host->multi_cfg->multimode :
                      ADC_Mode_Independent;
    ai.ADC_ScanConvMode = (host->config.sequencer_mode != 0) ? ENABLE : DISABLE;
    ai.ADC_ContinuousConvMode = (host->config.continuous_mode != 0) ? ENABLE : DISABLE;
    ai.ADC_ExternalTrigConv = (host->config.trigger_src != 0) ? (uint32_t)host->config.trigger_src :
                                                                ADC_ExternalTrigConv_None;
    ai.ADC_DataAlign =
        (host->config.align != 0) ? (uint32_t)host->config.align : ADC_DataAlign_Right;
    ai.ADC_NbrOfChannel = (host->config.channel_num > 0) ? (uint8_t)host->config.channel_num :
                                                           (uint8_t)host->channel_count;
    ai.ADC_OutputBuffer = (host->config.output_buf != 0) ? (uint32_t)host->config.output_buf :
                                                           ADC_OutputBuffer_Disable;
    ai.ADC_Pga = ADC_Pga_1;
    ADC_Init(adc, &ai);

    if (host->config.internal_ch_enable != 0)
        ADC_TempSensorVrefintCmd(ENABLE);
    ADC_Cmd(adc, ENABLE);

    return s_adc_calibrate(adc);
}

/**
 * @brief 关闭全部 ADC 通道并复位外设 (ADC_DeInit)
 * @param[in] pdev ADC 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_deinit_all_adcx(hal_adc_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ADC_TypeDef* adc = (ADC_TypeDef*)pdev->host->adc_handle;
    ADC_TempSensorVrefintCmd(DISABLE);
    ADC_DeInit(adc);
    return MINI_OK;
}

/**
 * @brief 关闭指定通道 (WCH F1 型无独立通道关闭寄存器, 未实现)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_adc_deinit_adcx_channel(hal_adc_device* pdev, uint32_t channel_id)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(channel_id);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 启动 ADC 转换 (软件触发, 连续模式下持续转换)
 * @param[in] pdev ADC 设备指针 (已绑定并完成 hal_adc_init)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_start(hal_adc_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ADC_TypeDef* adc = (ADC_TypeDef*)pdev->host->adc_handle;
    ADC_SoftwareStartConvCmd(adc, ENABLE);
    return MINI_OK;
}

/**
 * @brief 停止 ADC 转换并关闭外设
 * @param[in] pdev ADC 设备指针 (已绑定并完成 hal_adc_init)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_stop(hal_adc_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ADC_TypeDef* adc = (ADC_TypeDef*)pdev->host->adc_handle;
    ADC_SoftwareStartConvCmd(adc, DISABLE);
    ADC_Cmd(adc, DISABLE);
    return MINI_OK;
}

/**
 * @brief 读取指定通道转换值 (单通道轮询: 现配通道 + 软启动 + 等 EOC + 读 RDATAR)
 * @param[in] pdev ADC 设备指针 (已绑定并完成 hal_adc_init)
 * @param[in] channel_num 目标硬件通道号 (ADC_Channel_* 直投)
 * @param[out] out_val 转换结果输出指针 (12 位右对齐)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_read_value(hal_adc_device* pdev, uint32_t channel_num, uint16_t* out_val)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (out_val == NULL))
        return MINI_ERR_INVAL;
    hal_adc_host_config* host = pdev->host;
    ADC_TypeDef* adc = (ADC_TypeDef*)host->adc_handle;

    ADC_RegularChannelConfig(adc, (uint8_t)channel_num, 1U, s_adc_sample_time(host, channel_num));
    adc->RSQR1 = 0U; /* L=0: 规则组仅 1 个转换 */

    ADC_ClearFlag(adc, ADC_FLAG_EOC);
    ADC_SoftwareStartConvCmd(adc, ENABLE);

    uint32_t budget = ch32_poll_budget(10U);
    while (ADC_GetFlagStatus(adc, ADC_FLAG_EOC) == RESET)
    {
        if (budget == 0U)
            return MINI_ERR_TIMEOUT;
        budget--;
    }
    *out_val = ADC_GetConversionValue(adc);
    return MINI_OK;
}

/**
 * @brief 轮询等待当前转换完成 (消耗共享超时预算 10ms)
 * @param[in] pdev ADC 设备指针 (已绑定并完成 hal_adc_init)
 * @param[out] out_status 转换完成状态输出指针 (非 0=完成)
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_poll_for_conversion(hal_adc_device* pdev, uint32_t* out_status)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (out_status == NULL))
        return MINI_ERR_INVAL;
    ADC_TypeDef* adc = (ADC_TypeDef*)pdev->host->adc_handle;
    uint32_t budget = ch32_poll_budget(10U);
    while (ADC_GetFlagStatus(adc, ADC_FLAG_EOC) == RESET)
    {
        if (budget == 0U)
        {
            *out_status = 0U;
            return MINI_ERR_TIMEOUT;
        }
        budget--;
    }
    *out_status = 1U;
    return MINI_OK;
}

/**
 * @brief 获取 ADC 有效通道数量 (host->channel_count)
 * @param[in] pdev ADC 设备指针 (已绑定)
 * @param[out] count 通道数输出指针 (出参)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_count(hal_adc_device* pdev, uint32_t* count)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (count == NULL))
        return MINI_ERR_INVAL;
    *count = pdev->host->channel_count;
    return MINI_OK;
}

/**
 * @brief 获取指定索引通道的硬件通道号 (host->channels[] 查表)
 * @param[in] pdev ADC 设备指针 (已绑定)
 * @param[in] index 通道索引 (0-based)
 * @param[out] channel_id 硬件通道号输出指针 (出参)
 * @return 成功返回 MINI_OK, 索引越界或参数为空返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_id(hal_adc_device* pdev, int index, uint32_t* channel_id)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (channel_id == NULL))
        return MINI_ERR_INVAL;
    hal_adc_host_config* host = pdev->host;
    if ((index < 0) || ((uint32_t)index >= host->channel_count) || (host->channels == NULL))
        return MINI_ERR_INVAL;
    *channel_id = host->channels[index].channel_id;
    return MINI_OK;
}

/**
 * @brief 获取指定索引通道的采样时间 (host->channels[] 查表)
 * @param[in] pdev ADC 设备指针 (已绑定)
 * @param[in] index 通道索引 (0-based)
 * @param[out] sample_time 采样周期输出指针 (出参, ADC_SampleTime_* 宏值)
 * @return 成功返回 MINI_OK, 索引越界或参数为空返回 MINI_ERR_INVAL
 */
int hal_adc_get_channel_sample_time(hal_adc_device* pdev, int index, uint32_t* sample_time)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (sample_time == NULL))
        return MINI_ERR_INVAL;
    hal_adc_host_config* host = pdev->host;
    if ((index < 0) || ((uint32_t)index >= host->channel_count) || (host->channels == NULL))
        return MINI_ERR_INVAL;
    *sample_time = (host->channels[index].sample_time != 0U) ? host->channels[index].sample_time :
                                                               (uint32_t)ADC_SampleTime_239Cycles5;
    return MINI_OK;
}

/**
 * @brief 启动 ADC DMA 传输 (循环模式, 无中断标志)
 * @param[in] pdev ADC 设备指针 (已绑定 host 且已初始化)
 * @return 成功返回 MINI_OK, 错误码见 s_adc_dma_start
 */
int hal_adc_dma_start(hal_adc_device* pdev) { return s_adc_dma_start(pdev, false); }

/**
 * @brief 启动 ADC DMA 中断模式传输 (使能 DMA 传输完成中断标志,
 *        NVIC 使能与中断处理由板级中断子系统负责)
 * @param[in] pdev ADC 设备指针 (已绑定 host 且已初始化)
 * @return 成功返回 MINI_OK, 错误码见 s_adc_dma_start
 */
int hal_adc_dma_it_start(hal_adc_device* pdev) { return s_adc_dma_start(pdev, true); }

/**
 * @brief 读取 DMA 中断模式转换值 (取环形缓冲最新样本)
 * @param[in] pdev ADC 设备指针 (已调用过 dma_it_start)
 * @param[out] out_val 采样值输出指针 (出参)
 * @return 成功返回 MINI_OK, 未启动返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_dma_it_read_value(hal_adc_device* pdev, uint16_t* out_val)
{
    return s_adc_dma_read(pdev, out_val);
}

/**
 * @brief 读取 DMA 模式转换值 (取环形缓冲最新样本)
 * @param[in] pdev ADC 设备指针 (已调用过 dma_start)
 * @param[out] out_val 采样值输出指针 (出参)
 * @return 成功返回 MINI_OK, 未启动返回 MINI_ERR_NOTSUPP, 参数无效返回 MINI_ERR_INVAL
 */
int hal_adc_dma_read_value(hal_adc_device* pdev, uint16_t* out_val)
{
    return s_adc_dma_read(pdev, out_val);
}
