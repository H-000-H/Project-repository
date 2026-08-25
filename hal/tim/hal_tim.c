/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TIM HAL — STM32F1 实现 (LL 库)
 *
 * 设计: 上层接口 hal_tim.h 不变, 平台实现直接用 STM32 LL 库函数。
 * - 初始化照 stm32_slice 官方生成写法 (LL_TIM_InitTypeDef + LL_TIM_Init)。
 * - 强符号覆盖 mini_tree/hal/tim/hal_tim.c 的 weak 空桩。
 * - 参数从 hal_tim_host_config 结构体读出 (dtsi 提供 LL_TIM 宏值), 填 LL_TIM。
 * - F103xB 支持 TIM1/2/3/4 的 base/PWM/输入捕获/编码器; 霍尔(hall)无硬件映射返回 NOTSUPP。
 * - 不依赖 STM32 HAL 库, 仅依赖 LL 层头文件。
 */
#include "hal_tim.h"
#include "status.h"
#include "compiler_compat.h"

#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_tim.h"

#define HAL_TIM_F1_INSTANCES 4U

typedef struct hal_tim_f1_ctx
{
    bool       used;
    uintptr_t  base;      /* TIM 基址 */
    TIM_TypeDef* tim;     /* TIM 外设寄存器基址 */
} hal_tim_f1_ctx_t;

static hal_tim_f1_ctx_t s_ctx[HAL_TIM_F1_INSTANCES];

static hal_tim_f1_ctx_t* hal_tim_f1_find(uintptr_t base)
{
    uint32_t i;
    for (i = 0; i < HAL_TIM_F1_INSTANCES; i++)
        if (s_ctx[i].used && s_ctx[i].base == base)
            return &s_ctx[i];
    return NULL;
}

/** 按 TIM 基址使能时钟 (TIM1=APB2, TIM2/3/4=APB1) */
static void hal_tim_f1_enable_clock(uintptr_t base)
{
    if (base == TIM1_BASE)      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM1);
    else if (base == TIM2_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
    else if (base == TIM3_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
    else if (base == TIM4_BASE) LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4);
}

static hal_tim_f1_ctx_t* hal_tim_f1_get(const hal_tim_device* pdev)
{
    if (!pdev || !pdev->host)
        return NULL;
    return hal_tim_f1_find(pdev->host->tim_handle);
}

int hal_tim_device_init(hal_tim_device* pdev, hal_tim_platform_unique_config* unique,
                        hal_tim_host_config* host)
{
    hal_tim_f1_ctx_t* ctx;
    uint32_t i;

    if (!pdev || !host || !host->tim_handle)
        return VFS_ERR_INVAL;

    pdev->host   = host;
    pdev->unique = unique;

    ctx = hal_tim_f1_find(host->tim_handle);
    if (!ctx)
    {
        for (i = 0; i < HAL_TIM_F1_INSTANCES; i++)
        {
            if (!s_ctx[i].used) { ctx = &s_ctx[i]; break; }
        }
        if (!ctx)
            return VFS_ERR_NOMEM;
        COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
        ctx->used = true;
        ctx->base = host->tim_handle;
        ctx->tim  = (TIM_TypeDef*)host->tim_handle;
    }

    hal_tim_f1_enable_clock(host->tim_handle);

    /* 从 host->base 填时基参数 (dtsi 的 LL 宏值) */
    LL_TIM_InitTypeDef ti = {0};
    ti.Prescaler         = host->base.prescaler;
    ti.CounterMode       = host->base.counter_mode ?
                           host->base.counter_mode : LL_TIM_COUNTERMODE_UP;
    ti.Autoreload        = host->base.autoreload;
    ti.ClockDivision     = host->base.clock_division;
    ti.RepetitionCounter = host->base.repetition_counter;
    if (LL_TIM_Init(ctx->tim, &ti) != SUCCESS)
        return VFS_ERR_IO;

    /* 与官方 tim.c 一致的额外设置 */
    LL_TIM_DisableARRPreload(ctx->tim);
    LL_TIM_SetClockSource(ctx->tim, LL_TIM_CLOCKSOURCE_INTERNAL);
    LL_TIM_SetTriggerOutput(ctx->tim, LL_TIM_TRGO_RESET);
    LL_TIM_DisableMasterSlaveMode(ctx->tim);

    return VFS_OK;
}

int hal_tim_device_deinit(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_DisableCounter(ctx->tim);
    ctx->used = false;
    if (pdev)
        pdev->host = NULL;
    return VFS_OK;
}

int hal_tim_open(hal_tim_device* pdev)
{
    return pdev && pdev->host ? VFS_OK : VFS_ERR_INVAL;
}

int hal_tim_close(hal_tim_device* pdev)
{
    return pdev && pdev->host ? VFS_OK : VFS_ERR_INVAL;
}

int hal_tim_base_start(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;

    if (pdev->host->int_mask != 0U)
    {
        LL_TIM_EnableIT_UPDATE(ctx->tim);
        NVIC_SetPriority((IRQn_Type)pdev->host->irqn, pdev->host->irq_priority);
        NVIC_EnableIRQ((IRQn_Type)pdev->host->irqn);
    }
    LL_TIM_EnableCounter(ctx->tim);
    return VFS_OK;
}

int hal_tim_base_stop(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_DisableCounter(ctx->tim);
    if (pdev->host->int_mask != 0U)
        NVIC_DisableIRQ((IRQn_Type)pdev->host->irqn);
    return VFS_OK;
}

int hal_tim_force_stop(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_DisableCounter(ctx->tim);
    return VFS_OK;
}

int hal_tim_pwm_update(hal_tim_device* pdev, uint32_t channel, uint32_t frequency, uint32_t duty)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    TIM_TypeDef* tim;
    uint32_t arr;
    uint32_t ccr;

    if (!ctx || channel < 1U || channel > 4U)
        return VFS_ERR_INVAL;

    tim = ctx->tim;

    /* 更新 ARR: 若指定频率则重算 (ARR = clk/(PSC+1)/freq - 1); F1 上 TIM1=PCLK2, 其余=PCLK1, 默认均 72MHz */
    if (frequency != 0U)
    {
        uint32_t clk = SystemCoreClock; /* F103 默认 72MHz, PCLK1/PCLK2 倍频后均=72MHz */
        arr = (clk / ((LL_TIM_GetPrescaler(tim) + 1U) * frequency)) - 1U;
        LL_TIM_SetAutoReload(tim, arr);
    }
    else
        arr = LL_TIM_GetAutoReload(tim);

    /* CCR = duty (若 duty <= ARR 视为计数, 否则按百分比) */
    ccr = (duty > arr) ? (arr * duty) / 100U : duty;
    switch (channel)
    {
    case 1U: LL_TIM_OC_SetCompareCH1(tim, ccr); break;
    case 2U: LL_TIM_OC_SetCompareCH2(tim, ccr); break;
    case 3U: LL_TIM_OC_SetCompareCH3(tim, ccr); break;
    default: LL_TIM_OC_SetCompareCH4(tim, ccr); break;
    }

    return VFS_OK;
}

int hal_tim_interrupt_config(hal_tim_device* pdev, uint32_t interrupt_config)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;

    if (interrupt_config & (1UL << 0))
        LL_TIM_EnableIT_UPDATE(ctx->tim);
    else
        LL_TIM_DisableIT_UPDATE(ctx->tim);
    return VFS_OK;
}

int hal_tim_get_counter(const hal_tim_device* pdev, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value)
        return VFS_ERR_INVAL;
    *value = LL_TIM_GetCounter(ctx->tim);
    return VFS_OK;
}

int hal_tim_get_capture_value(const hal_tim_device* pdev, uint32_t channel, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value || channel < 1U || channel > 4U)
        return VFS_ERR_INVAL;
    switch (channel)
    {
    case 1U: *value = LL_TIM_OC_GetCompareCH1(ctx->tim); break;
    case 2U: *value = LL_TIM_OC_GetCompareCH2(ctx->tim); break;
    case 3U: *value = LL_TIM_OC_GetCompareCH3(ctx->tim); break;
    default: *value = LL_TIM_OC_GetCompareCH4(ctx->tim); break;
    }
    return VFS_OK;
}

int hal_tim_get_encoder_value(const hal_tim_device* pdev, uint32_t* value)
{
    return hal_tim_get_counter(pdev, value);
}

int hal_tim_get_hall_value(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(value);
    return VFS_ERR_NOTSUPP; /* F103 无霍尔硬件映射 */
}

int hal_tim_encoder_start(hal_tim_device* pdev, uint32_t encoder_mode)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    TIM_TypeDef* tim;
    uint32_t mode;

    if (!ctx)
        return VFS_ERR_INVAL;
    tim = ctx->tim;

    /* 编码器模式: 1/2/4 倍频 → TIM_ENCODERMODE_TI1/TI2/TI12 (SMCR.SMS 位定义, F1 用 HAL 名) */
    mode = (encoder_mode <= 1U) ? TIM_ENCODERMODE_TI1 :
           (encoder_mode == 2U) ? TIM_ENCODERMODE_TI2 :
                                  TIM_ENCODERMODE_TI12;

    /* 直接配置 SMCR(编码器模式) + CCMR1(CC1/CC2 映射为 TI1/TI2) */
    MODIFY_REG(tim->SMCR, TIM_SMCR_SMS, mode);
    MODIFY_REG(tim->CCMR1, (TIM_CCMR1_CC1S | TIM_CCMR1_CC2S), 0x00000011U);
    CLEAR_BIT(tim->CCMR1, (TIM_CCMR1_IC1F | TIM_CCMR1_IC2F));
    CLEAR_BIT(tim->CCMR1, (TIM_CCMR1_IC1PSC | TIM_CCMR1_IC2PSC));
    CLEAR_BIT(tim->CCER, (TIM_CCER_CC1P | TIM_CCER_CC2P));

    LL_TIM_EnableCounter(tim);
    return VFS_OK;
}

int hal_tim_hall_start(hal_tim_device* pdev)
{
    COMPAT_UNUSED_PARAM(pdev);
    return VFS_ERR_NOTSUPP; /* F103 无霍尔硬件映射 */
}

int hal_tim_set_counter(hal_tim_device* pdev, uint32_t value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_SetCounter(ctx->tim, value);
    return VFS_OK;
}

int hal_tim_set_autoreload(hal_tim_device* pdev, uint32_t value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_SetAutoReload(ctx->tim, value);
    return VFS_OK;
}

int hal_tim_get_autoreload(const hal_tim_device* pdev, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value)
        return VFS_ERR_INVAL;
    *value = LL_TIM_GetAutoReload(ctx->tim);
    return VFS_OK;
}

int hal_tim_set_prescaler(hal_tim_device* pdev, uint32_t value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_SetPrescaler(ctx->tim, value);
    return VFS_OK;
}

int hal_tim_get_prescaler(const hal_tim_device* pdev, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value)
        return VFS_ERR_INVAL;
    *value = LL_TIM_GetPrescaler(ctx->tim);
    return VFS_OK;
}

int hal_tim_set_clock_division(hal_tim_device* pdev, uint32_t value)
{
    COMPAT_UNUSED_PARAM(pdev);
    COMPAT_UNUSED_PARAM(value);
    return VFS_ERR_NOTSUPP; /* F1 时钟分频由 LL_TIM_Init 设置, 运行时不改 */
}

int hal_tim_get_clock_division(const hal_tim_device* pdev, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value)
        return VFS_ERR_INVAL;
    *value = READ_BIT(ctx->tim->CR1, TIM_CR1_CKD); /* CKD[9:8] */
    return VFS_OK;
}

int hal_tim_set_counter_mode(hal_tim_device* pdev, uint32_t value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_SetCounterMode(ctx->tim, value);
    return VFS_OK;
}

int hal_tim_get_counter_mode(const hal_tim_device* pdev, uint32_t* value)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx || !value)
        return VFS_ERR_INVAL;
    *value = LL_TIM_GetCounterMode(ctx->tim);
    return VFS_OK;
}

int hal_tim_enable_arr_preload(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_EnableARRPreload(ctx->tim);
    return VFS_OK;
}

int hal_tim_disable_arr_preload(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_DisableARRPreload(ctx->tim);
    return VFS_OK;
}

int hal_tim_clear_update_flag(hal_tim_device* pdev)
{
    hal_tim_f1_ctx_t* ctx = hal_tim_f1_get(pdev);
    if (!ctx)
        return VFS_ERR_INVAL;
    LL_TIM_ClearFlag_UPDATE(ctx->tim);
    return VFS_OK;
}
