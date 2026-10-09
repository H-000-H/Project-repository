/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_tim_ch32v30x.c
 *@brief CH32V307 TIM HAL 强符号实现 (覆盖 mini_tree/hal/tim/hal_tim.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头。
 *   直投约定 (对齐 hal_tim.h):
 *   - tim_handle = TIMx_BASE, clk_periph = RCC_APBxPeriph_TIMx;
 *   - base.counter_mode / clock_division 与 OC/IC 各字段直投 WCH TIM_* 宏;
 *   - 热路径读改写寄存器 (CNT/PSC/ATRLR/CHxCVR), 冷路径走标准外设库;
 *   - PWM 输出按 active_chn_mask 配置 oc_mode 联合体的通道表;
 *   - 编码器仅支持 1/2/4 倍频 (SMCFGR.SMS); 霍尔模式未实现 (NOTSUPP);
 *   - 中断回调注册走板级中断子系统, hal_tim_interrupt_config 仅灌 DMAINTENR。
 */

#include "ch32v30x_hal_common.h"
#include "hal_tim.h"
#include <string.h>

/* =============================================================================
 * 内部辅助 (引用计数 / 通道寄存器定位 / OC 配置 / 硬件初始化)
 * ============================================================================= */

/* host 级引用计数 (同一 TIM 被多设备打开时只初始化/关闭一次) */
#define CH32_TIM_REF_SLOTS 4

typedef struct
{
    uintptr_t base;
    int ref_count;
} ch32_tim_ref_t;

static ch32_tim_ref_t s_tim_ref[CH32_TIM_REF_SLOTS];

/**
 * @brief 查找/分配引用计数槽位 (已存在则复用, 否则取第一个空闲槽)
 * @param[in] base TIMx_BASE (唯一键)
 * @return 槽位指针, 槽位用尽返回 NULL
 */
static ch32_tim_ref_t* s_ref_acquire(uintptr_t base)
{
    ch32_tim_ref_t* free_slot = NULL;
    for (size_t i = 0; i < CH32_TIM_REF_SLOTS; i++)
    {
        if (s_tim_ref[i].base == base)
            return &s_tim_ref[i];
        if ((free_slot == NULL) && (s_tim_ref[i].ref_count == 0))
            free_slot = &s_tim_ref[i];
    }
    if (free_slot != NULL)
    {
        free_slot->base = base;
        free_slot->ref_count = 0;
    }
    return free_slot;
}

/**
 * @brief 释放引用计数 (归零后清空槽位键)
 * @param[in] base TIMx_BASE (唯一键)
 */
static void s_ref_release(uintptr_t base)
{
    for (size_t i = 0; i < CH32_TIM_REF_SLOTS; i++)
    {
        if ((s_tim_ref[i].base == base) && (s_tim_ref[i].ref_count > 0))
        {
            s_tim_ref[i].ref_count--;
            if (s_tim_ref[i].ref_count == 0)
                s_tim_ref[i].base = 0U;
            return;
        }
    }
}

/**
 * @brief 通道号 1..4 → CCR 寄存器指针 (热路径直读直写)
 * @param[in] tim TIM 外设寄存器指针 (出参, 仅取寄存器地址)
 * @param[in] channel 通道号 (1..4)
 * @return CCR 寄存器指针, 通道号无效返回 NULL
 */
static volatile uint16_t* s_tim_ccr(TIM_TypeDef* tim, uint32_t channel)
{
    switch (channel)
    {
    case 1U:
        return &tim->CH1CVR;
    case 2U:
        return &tim->CH2CVR;
    case 3U:
        return &tim->CH3CVR;
    case 4U:
        return &tim->CH4CVR;
    default:
        return NULL;
    }
}

/**
 * @brief OC 通道初始化 (PWM/输出比较), 含引脚直投配置 (仅配 active_chn_mask 置位的通道)
 * @param[in] tim TIM 外设寄存器指针 (出参, 写 OC 寄存器)
 * @param[in] host TIM 主机配置 (出参, 读 oc_mode 通道表与引脚表)
 * @return 成功返回 MINI_OK, 引脚配置失败透传错误码
 */
static int s_tim_oc_setup(TIM_TypeDef* tim, hal_tim_host_config* host)
{
    for (uint32_t ch = 1U; ch <= 4U; ch++)
    {
        if ((host->active_chn_mask & ((uint32_t)1U << ch)) == 0U)
            continue;
        uint32_t idx = ch - 1U;
        const hal_output_compare_config* oc = &host->oc_mode.config[idx];
        const hal_tim_channel_config* chn = &host->oc_mode.channel[idx];
        const hal_tim_pin_config* pin = &host->oc_mode.pin[idx];

        TIM_OCInitTypeDef oci;
        memset(&oci, 0, sizeof(oci));
        oci.TIM_OCMode = (oc->oc_mode != 0U) ? (uint16_t)oc->oc_mode : (uint16_t)TIM_OCMode_PWM1;
        oci.TIM_OutputState =
            (oc->oc_state != 0U) ? (uint16_t)oc->oc_state : (uint16_t)TIM_OutputState_Enable;
        oci.TIM_OutputNState = (chn->enable_complementary != 0U) ?
                                   ((oc->oc_n_state != 0U) ? (uint16_t)oc->oc_n_state :
                                                             (uint16_t)TIM_OutputNState_Enable) :
                                   (uint16_t)TIM_OutputNState_Disable;
        oci.TIM_Pulse = oc->compare_value;
        oci.TIM_OCPolarity =
            (oc->oc_polarity != 0U) ? (uint16_t)oc->oc_polarity : (uint16_t)TIM_OCPolarity_High;
        oci.TIM_OCNPolarity = (oc->oc_n_polarity != 0U) ? (uint16_t)oc->oc_n_polarity :
                                                          (uint16_t)TIM_OCNPolarity_High;
        oci.TIM_OCIdleState = (uint16_t)oc->oc_idle_state;
        oci.TIM_OCNIdleState = (uint16_t)oc->oc_n_idle_state;

        switch (ch)
        {
        case 1U:
            TIM_OC1Init(tim, &oci);
            TIM_OC1PreloadConfig(tim, (uint16_t)TIM_OCPreload_Enable);
            break;
        case 2U:
            TIM_OC2Init(tim, &oci);
            TIM_OC2PreloadConfig(tim, (uint16_t)TIM_OCPreload_Enable);
            break;
        case 3U:
            TIM_OC3Init(tim, &oci);
            TIM_OC3PreloadConfig(tim, (uint16_t)TIM_OCPreload_Enable);
            break;
        default:
            TIM_OC4Init(tim, &oci);
            TIM_OC4PreloadConfig(tim, (uint16_t)TIM_OCPreload_Enable);
            break;
        }

        /* 引脚: af 承载 GPIOMode_TypeDef (缺省复用推挽) */
        if (pin->port != 0U)
        {
            int ret = ch32_gpio_port_clk(pin->port, pin->clk_bus);
            if (ret != MINI_OK)
                return ret;
            uint32_t mode = (pin->mode != 0U) ? pin->mode : pin->af;
            if (mode == 0U)
                mode = (uint32_t)GPIO_Mode_AF_PP;
            ret = ch32_gpio_cfg_pin(pin->port, ch32_pin_to_mask(pin->pin), mode, pin->speed,
                                    pin->pull);
            if (ret != MINI_OK)
                return ret;
        }
    }
    return MINI_OK;
}

/**
 * @brief 首次打开: 时钟 + 时基 + 通道 + 高级定时器输出使能 (一次完成)
 * @param[in] host TIM 主机配置 (出参, 读全部直投字段)
 * @return 成功返回 MINI_OK, 时钟/引脚失败透传错误码
 */
static int s_tim_hw_setup(hal_tim_host_config* host)
{
    uintptr_t base = host->tim_handle;
    TIM_TypeDef* tim = (TIM_TypeDef*)base;

    int ret = ch32_rcc_enable(base, host->clk_periph);
    if (ret != MINI_OK)
        return ret;

    TIM_TimeBaseInitTypeDef tb;
    memset(&tb, 0, sizeof(tb));
    tb.TIM_Prescaler = (uint16_t)host->base.prescaler;
    tb.TIM_CounterMode = (host->base.counter_mode != 0U) ? (uint16_t)host->base.counter_mode :
                                                           (uint16_t)TIM_CounterMode_Up;
    tb.TIM_Period = host->base.autoreload;
    tb.TIM_ClockDivision = (uint16_t)host->base.clock_division;
    tb.TIM_RepetitionCounter = (uint8_t)host->base.repetition_counter;
    TIM_TimeBaseInit(tim, &tb);

    ret = s_tim_oc_setup(tim, host);
    if (ret != MINI_OK)
        return ret;

    /* BDTR (高级定时器): 直投死区/刹车, 非高级定时器写忽略 */
    tim->BDTR = host->bdtr.dead_time & 0xFFU;
    if (host->bdtr.break_state != 0U)
        tim->BDTR |= (uint32_t)0x1000U; /* BKE */
    if (host->bdtr.automatic_output != 0U)
        tim->BDTR |= (uint32_t)0x4000U; /* AOE */
    tim->BDTR |= (uint32_t)0x8000U; /* MOE: 主输出使能 */

    /* 中断使能掩码直灌 (0 = 不使能) */
    if (host->int_mask != 0U)
        tim->DMAINTENR |= (uint32_t)host->int_mask;

    TIM_ClearFlag(tim, TIM_FLAG_Update);
    return MINI_OK;
}

/* -------------------------------------------------------------------------- */

/* =============================================================================
 * 上层接口 (hal_tim.h)
 * ============================================================================= */

/**
 * @brief 绑定 TIM 设备与主机/平台配置 (不碰硬件, 打开时才初始化)
 * @param[in] pdev TIM 设备指针 (出参)
 * @param[in] unique 平台唯一配置 (出参, private_cfg 写入 TIMx_BASE)
 * @param[in] host TIM 主机配置 (出参, 仅保存指针)
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_tim_device_init(hal_tim_device* pdev, hal_tim_platform_unique_config* unique,
                        hal_tim_host_config* host)
{
    if ((pdev == NULL) || (host == NULL))
        return MINI_ERR_INVAL;
    pdev->host = host;
    pdev->unique = unique;
    if (unique != NULL)
        unique->private_cfg = host->tim_handle;
    return MINI_OK;
}

/**
 * @brief 释放 TIM 设备运行时资源 (仅解绑定, 不关硬件)
 * @param[in] pdev TIM 设备指针 (出参)
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_tim_device_deinit(hal_tim_device* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    pdev->host = NULL;
    pdev->unique = NULL;
    return MINI_OK;
}

/**
 * @brief 打开 TIM (首次引用执行时钟/时基/通道初始化, 重复打开仅累加计数)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 槽位用尽返回 MINI_ERR_NOMEM, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_open(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ch32_tim_ref_t* slot = s_ref_acquire(pdev->host->tim_handle);
    if (slot == NULL)
        return MINI_ERR_NOMEM;
    if (slot->ref_count == 0)
    {
        int ret = s_tim_hw_setup(pdev->host);
        if (ret != MINI_OK)
            return ret;
    }
    slot->ref_count++;
    return MINI_OK;
}

/**
 * @brief 关闭 TIM (引用归零后去使能计数器)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_close(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    uintptr_t base = pdev->host->tim_handle;
    s_ref_release(base);
    ch32_tim_ref_t* slot = s_ref_acquire(base);
    if ((slot == NULL) || (slot->ref_count == 0))
        TIM_Cmd((TIM_TypeDef*)base, DISABLE);
    return MINI_OK;
}

/**
 * @brief PWM 频率/占空比热更新 (重算 PSC/ARR, duty<=100 视为百分比, UG 立即装载)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] channel 通道号 (1..4)
 * @param[in] frequency 目标 PWM 频率 (Hz, 按 ch32_tim_input_freq 折算)
 * @param[in] duty 占空比: <=100 为百分比, 否则为原始比较值 (截断到 ARR)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_pwm_update(hal_tim_device* pdev, uint32_t channel, uint32_t frequency, uint32_t duty)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    volatile uint16_t* ccr = s_tim_ccr((TIM_TypeDef*)pdev->host->tim_handle, channel);
    if ((ccr == NULL) || (frequency == 0U))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;

    uint32_t fsrc = ch32_tim_input_freq(pdev->host->tim_handle);
    uint32_t psc = (fsrc / frequency) / 65536U; /* 保证 ARR 落在 16 位内 */
    uint32_t arr = (fsrc / ((psc + 1U) * frequency)) - 1U;
    if (arr == 0U)
        return MINI_ERR_INVAL;
    tim->PSC = psc;
    tim->ATRLR = arr;

    /* duty <= 100 视为百分比, 否则视为原始比较值 (截断到 ARR) */
    uint32_t pulse = (duty <= 100U) ? ((arr * duty) / 100U) : duty;
    if (pulse > arr)
        pulse = arr;
    *ccr = (uint16_t)pulse;

    tim->SWEVGR = (uint32_t)TIM_PSCReloadMode_Immediate; /* UG: 立即装载 PSC/ARR */
    return MINI_OK;
}

/**
 * @brief 中断配置 (直灌 WCH TIM_IT_* 位掩码到 DMAINTENR, NVIC 由板级中断子系统负责)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] interrupt_config TIM_IT_* 位掩码 (直投)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_interrupt_config(hal_tim_device* pdev, uint32_t interrupt_config)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    /* 直灌 WCH TIM_IT_* 位掩码 (NVIC 使能由板级中断子系统负责) */
    tim->DMAINTENR |= interrupt_config;
    return MINI_OK;
}

/**
 * @brief 读取计数器当前值 (CNT 直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 计数器值输出指针 (出参)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_counter(const hal_tim_device* pdev, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    const TIM_TypeDef* tim = (const TIM_TypeDef*)pdev->host->tim_handle;
    *value = tim->CNT;
    return MINI_OK;
}

/**
 * @brief 读取指定通道捕获值 (CHxCVR 直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] channel 通道号 (1..4)
 * @param[out] value 捕获值输出指针 (出参)
 * @return 成功返回 MINI_OK, 通道无效或参数为空返回 MINI_ERR_INVAL
 */
int hal_tim_get_capture_value(const hal_tim_device* pdev, uint32_t channel, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    const volatile uint16_t* ccr = s_tim_ccr((TIM_TypeDef*)pdev->host->tim_handle, channel);
    if (ccr == NULL)
        return MINI_ERR_INVAL;
    *value = *ccr;
    return MINI_OK;
}

/**
 * @brief 读取编码器计数值 (编码器模式下 CNT 即计数, 复用 get_counter)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 编码器计数输出指针 (出参)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_encoder_value(const hal_tim_device* pdev, uint32_t* value)
{
    return hal_tim_get_counter(pdev, value);
}

/**
 * @brief 读取霍尔传感器计数值 (霍尔模式未实现)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_tim_get_hall_value(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(value);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 强制停止 (关闭全部比较输出 + 去使能计数器)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_force_stop(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    tim->CCER = 0U; /* 关闭全部比较输出 */
    TIM_Cmd(tim, DISABLE);
    return MINI_OK;
}

/**
 * @brief 启动编码器模式 (1/2/4 倍频映射到 SMCFGR.SMS 0x1/0x2/0x3)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] encoder_mode 倍频数 (仅支持 1/2/4)
 * @return 成功返回 MINI_OK, 倍频数非法或参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_encoder_start(hal_tim_device* pdev, uint32_t encoder_mode)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    uint32_t sms;
    switch (encoder_mode)
    {
    case 1U:
        sms = 0x1U; /* 仅 TI1 边沿计数 */
        break;
    case 2U:
        sms = 0x2U; /* 仅 TI2 边沿计数 */
        break;
    case 4U:
        sms = 0x3U; /* TI1 + TI2 双沿计数 */
        break;
    default:
        return MINI_ERR_INVAL;
    }
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    tim->SMCFGR = (tim->SMCFGR & ~(uint32_t)0x7U) | sms;
    TIM_Cmd(tim, ENABLE);
    return MINI_OK;
}

/**
 * @brief 启动霍尔传感器模式 (未实现)
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP
 */
int hal_tim_hall_start(hal_tim_device* pdev)
{
    COMPAT_UNUSED_PARAM(pdev);
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 设置计数器当前值 (CNT 直写)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] value 目标计数值 (直投)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_set_counter(hal_tim_device* pdev, uint32_t value)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ((TIM_TypeDef*)pdev->host->tim_handle)->CNT = value;
    return MINI_OK;
}

/**
 * @brief 设置自动重装载值 (ATRLR 直写)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] value 目标重装载值 (直投)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_set_autoreload(hal_tim_device* pdev, uint32_t value)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ((TIM_TypeDef*)pdev->host->tim_handle)->ATRLR = value;
    return MINI_OK;
}

/**
 * @brief 读取自动重装载值 (ATRLR 直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 重装载值输出指针 (出参)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_autoreload(const hal_tim_device* pdev, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    *value = ((const TIM_TypeDef*)pdev->host->tim_handle)->ATRLR;
    return MINI_OK;
}

/**
 * @brief 设置预分频值 (PSC 直写)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] value 目标预分频值 (直投)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_set_prescaler(hal_tim_device* pdev, uint32_t value)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    ((TIM_TypeDef*)pdev->host->tim_handle)->PSC = value;
    return MINI_OK;
}

/**
 * @brief 读取预分频值 (PSC 直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 预分频值输出指针 (出参)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_prescaler(const hal_tim_device* pdev, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    *value = ((const TIM_TypeDef*)pdev->host->tim_handle)->PSC;
    return MINI_OK;
}

/**
 * @brief 设置时钟分频 (CTLR1.CKD 位段, WCH TIM_CKD_* 宏直投)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] value TIM_CKD_DIV1/DIV2/DIV4 宏值 (仅低 0x300 位生效)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_set_clock_division(hal_tim_device* pdev, uint32_t value)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    /* CKD 位于 CTLR1[9:8], WCH TIM_CKD_* 宏已对齐该位段 */
    tim->CTLR1 = (uint16_t)((tim->CTLR1 & ~(uint32_t)0x300U) | (value & 0x300U));
    return MINI_OK;
}

/**
 * @brief 读取时钟分频 (CTLR1.CKD 位段直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 分频值输出指针 (出参, 已对齐位段)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_clock_division(const hal_tim_device* pdev, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    *value = ((const TIM_TypeDef*)pdev->host->tim_handle)->CTLR1 & 0x300U;
    return MINI_OK;
}

/**
 * @brief 设置计数模式 (CTLR1.CMS/DIR 位段, WCH TIM_CounterMode_* 宏直投)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[in] value TIM_CounterMode_Up/Down/CenterAligned* 宏值
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_set_counter_mode(hal_tim_device* pdev, uint32_t value)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    /* CMS 位于 CTLR1[6:5]; DIR(bit4) 随 TIM_CounterMode_Down 语义置位 */
    uint32_t cms = value & 0x60U;
    uint32_t dir = (value == (uint32_t)TIM_CounterMode_Down) ? 0x10U : 0U;
    tim->CTLR1 = (uint16_t)((tim->CTLR1 & ~(uint32_t)0x70U) | cms | dir);
    return MINI_OK;
}

/**
 * @brief 读取计数模式 (CTLR1.CMS/DIR 位段直读)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @param[out] value 计数模式输出指针 (出参, 已对齐位段)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_get_counter_mode(const hal_tim_device* pdev, uint32_t* value)
{
    if ((pdev == NULL) || (pdev->host == NULL) || (value == NULL))
        return MINI_ERR_INVAL;
    uint32_t ctlr1 = ((const TIM_TypeDef*)pdev->host->tim_handle)->CTLR1;
    *value = ctlr1 & 0x70U;
    return MINI_OK;
}

/**
 * @brief 使能 ATRLR 预装载 (CTLR1.ARPE 置位)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_enable_arr_preload(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    tim->CTLR1 |= (uint32_t)0x80U; /* ARPE */
    return MINI_OK;
}

/**
 * @brief 关闭 ATRLR 预装载 (CTLR1.ARPE 清零)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_disable_arr_preload(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    tim->CTLR1 &= ~(uint32_t)0x80U;
    return MINI_OK;
}

/**
 * @brief 启动计数器 (TIM_Cmd ENABLE)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_base_start(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_Cmd((TIM_TypeDef*)pdev->host->tim_handle, ENABLE);
    return MINI_OK;
}

/**
 * @brief 停止计数器 (TIM_Cmd DISABLE)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_base_stop(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_Cmd((TIM_TypeDef*)pdev->host->tim_handle, DISABLE);
    return MINI_OK;
}

/**
 * @brief 清除 update 标志 (无标志可清时返回 IO 错误, 防 spurious IRQ)
 * @param[in] pdev TIM 设备指针 (已绑定 host)
 * @return 成功返回 MINI_OK, 无标志返回 MINI_ERR_IO, 参数无效返回 MINI_ERR_INVAL
 */
int hal_tim_clear_update_flag(hal_tim_device* pdev)
{
    if ((pdev == NULL) || (pdev->host == NULL))
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    if ((tim->INTFR & (uint32_t)TIM_FLAG_Update) == 0U)
    {
        /* spurious IRQ: 无 update 标志可清 */
        return MINI_ERR_IO;
    }
    TIM_ClearFlag(tim, TIM_FLAG_Update);
    return MINI_OK;
}
