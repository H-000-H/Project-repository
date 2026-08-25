/**
 * SPDX-License-Identifier: Apache-2.0
 * @brief ESP32-S3 TIM HAL — LEDC 作 PWM/OC 后端
 *
 * DTS 映射:
 *   hw-instance   → LEDC timer 编号
 *   clk-periph    → PWM 频率 Hz
 *   autoreload    → 占空比分辨率上限 (如 1023 → 10-bit)
 *   ocN-pin[1]    → GPIO
 *   ocN-channel-id→ LEDC channel
 */
#include "hal_tim.h"
#include "status.h"
#include "compiler_compat.h"

#include "driver/ledc.h"

#ifndef HAL_TIM_ESP_MAX
#define HAL_TIM_ESP_MAX  4
#endif

struct esp_tim_runtime
{
    int               used;
    ledc_mode_t       speed_mode;
    ledc_timer_t      timer;
    ledc_channel_t    channel;
    ledc_timer_bit_t  duty_res;
    uint32_t          freq_hz;
    uint32_t          arr;
    int               gpio;
};

static struct esp_tim_runtime s_tim_rt[HAL_TIM_ESP_MAX];

static ledc_timer_bit_t esp_tim_res_from_arr(uint32_t arr)
{
    if (arr >= 16383U)
        return LEDC_TIMER_14_BIT;
    if (arr >= 8191U)
        return LEDC_TIMER_13_BIT;
    if (arr >= 4095U)
        return LEDC_TIMER_12_BIT;
    if (arr >= 2047U)
        return LEDC_TIMER_11_BIT;
    if (arr >= 1023U)
        return LEDC_TIMER_10_BIT;
    if (arr >= 511U)
        return LEDC_TIMER_9_BIT;
    return LEDC_TIMER_8_BIT;
}

static struct esp_tim_runtime* esp_tim_rt_of(hal_tim_device* pdev)
{
    if (!pdev || !pdev->unique)
        return NULL;
    return (struct esp_tim_runtime*)pdev->unique->private_cfg;
}

int hal_tim_device_init(hal_tim_device* pdev, hal_tim_platform_unique_config* unique, hal_tim_host_config* host)
{
    int i;

    if (!pdev || !unique || !host)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->unique = unique;
    pdev->host   = host;

    for (i = 0; i < HAL_TIM_ESP_MAX; i++)
    {
        if (!s_tim_rt[i].used)
        {
            COMPAT_MEM_SET(&s_tim_rt[i], 0, sizeof(s_tim_rt[i]));
            s_tim_rt[i].used = 1;
            unique->private_cfg = (uintptr_t)&s_tim_rt[i];
            return VFS_OK;
        }
    }
    return VFS_ERR_NOMEM;
}

int hal_tim_device_deinit(hal_tim_device* pdev)
{
    struct esp_tim_runtime* rt = esp_tim_rt_of(pdev);

    if (rt)
        COMPAT_MEM_SET(rt, 0, sizeof(*rt));
    if (pdev)
    {
        if (pdev->unique)
            pdev->unique->private_cfg = 0;
        pdev->unique = NULL;
        pdev->host   = NULL;
    }
    return VFS_OK;
}

int hal_tim_open(hal_tim_device* pdev)
{
    struct esp_tim_runtime* rt;
    ledc_timer_config_t     tcfg;
    ledc_channel_config_t   ccfg;
    uint32_t                ch_idx = 0;
    uint32_t                i;

    if (!pdev || !pdev->host || !pdev->unique)
        return VFS_ERR_INVAL;

    rt = esp_tim_rt_of(pdev);
    if (!rt)
        return VFS_ERR_INVAL;

    if (pdev->host->mode != HAL_TIM_MODE_OC)
        return VFS_ERR_NOTSUPP;

    for (i = 0; i < HAL_OUTPUT_COMPARE_TIM_MAX_CHANNELS; i++)
    {
        if (pdev->host->active_chn_mask & (1U << i))
        {
            ch_idx = i;
            break;
        }
    }

    rt->speed_mode = LEDC_LOW_SPEED_MODE;
    rt->timer      = (ledc_timer_t)((unsigned)pdev->host->tim_handle % LEDC_TIMER_MAX);
    rt->channel    = (ledc_channel_t)(pdev->host->oc_mode.channel[ch_idx].channel_id
                                      % LEDC_CHANNEL_MAX);
    rt->arr        = pdev->host->base.autoreload ? pdev->host->base.autoreload : 1023U;
    rt->duty_res   = esp_tim_res_from_arr(rt->arr);
    rt->freq_hz    = pdev->host->clk_periph ? pdev->host->clk_periph : 5000U;
    rt->gpio       = (int)pdev->host->oc_mode.pin[ch_idx].pin;

    COMPAT_MEM_SET(&tcfg, 0, sizeof(tcfg));
    tcfg.speed_mode      = rt->speed_mode;
    tcfg.duty_resolution = rt->duty_res;
    tcfg.timer_num       = rt->timer;
    tcfg.freq_hz         = rt->freq_hz;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tcfg) != ESP_OK)
        return VFS_ERR_IO;

    COMPAT_MEM_SET(&ccfg, 0, sizeof(ccfg));
    ccfg.gpio_num   = rt->gpio;
    ccfg.speed_mode = rt->speed_mode;
    ccfg.channel    = rt->channel;
    ccfg.timer_sel  = rt->timer;
    ccfg.duty       = 0;
    ccfg.hpoint     = 0;
    if (ledc_channel_config(&ccfg) != ESP_OK)
        return VFS_ERR_IO;

    return VFS_OK;
}

int hal_tim_close(hal_tim_device* pdev)
{
    struct esp_tim_runtime* rt = esp_tim_rt_of(pdev);

    if (!rt)
        return VFS_ERR_INVAL;
    COMPAT_IGNORE_RESULT(ledc_stop(rt->speed_mode, rt->channel, 0));
    return VFS_OK;
}

int hal_tim_pwm_update(hal_tim_device* pdev, uint32_t channel, uint32_t frequency, uint32_t duty)
{
    struct esp_tim_runtime* rt = esp_tim_rt_of(pdev);
    uint32_t                max_duty;

    COMPAT_IGNORE_RESULT(channel);
    COMPAT_IGNORE_RESULT(frequency);
    if (!rt)
        return VFS_ERR_INVAL;

    max_duty = (1U << (unsigned)rt->duty_res) - 1U;
    if (duty > max_duty)
        duty = max_duty;

    if (ledc_set_duty(rt->speed_mode, rt->channel, duty) != ESP_OK)
        return VFS_ERR_IO;
    if (ledc_update_duty(rt->speed_mode, rt->channel) != ESP_OK)
        return VFS_ERR_IO;
    return VFS_OK;
}

int hal_tim_interrupt_config(hal_tim_device* pdev, uint32_t interrupt_config)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(interrupt_config);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_counter(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_capture_value(const hal_tim_device* pdev, uint32_t channel, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(channel);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_encoder_value(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_hall_value(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_force_stop(hal_tim_device* pdev)
{
    return hal_tim_close(pdev);
}

int hal_tim_encoder_start(hal_tim_device* pdev, uint32_t encoder_mode)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(encoder_mode);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_hall_start(hal_tim_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_set_counter(hal_tim_device* pdev, uint32_t value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_set_autoreload(hal_tim_device* pdev, uint32_t value)
{
    struct esp_tim_runtime* rt = esp_tim_rt_of(pdev);

    if (!rt)
        return VFS_ERR_INVAL;
    rt->arr = value ? value : rt->arr;
    return VFS_OK;
}

int hal_tim_get_autoreload(const hal_tim_device* pdev, uint32_t* value)
{
    struct esp_tim_runtime* rt = esp_tim_rt_of((hal_tim_device*)pdev);

    if (!rt || !value)
        return VFS_ERR_INVAL;
    *value = rt->arr;
    return VFS_OK;
}

int hal_tim_set_prescaler(hal_tim_device* pdev, uint32_t value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_prescaler(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_set_clock_division(hal_tim_device* pdev, uint32_t value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_clock_division(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_set_counter_mode(hal_tim_device* pdev, uint32_t value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_get_counter_mode(const hal_tim_device* pdev, uint32_t* value)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(value);
    return VFS_ERR_NOTSUPP;
}

int hal_tim_enable_arr_preload(hal_tim_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_OK;
}

int hal_tim_disable_arr_preload(hal_tim_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_OK;
}

int hal_tim_base_start(hal_tim_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_OK;
}

int hal_tim_base_stop(hal_tim_device* pdev)
{
    return hal_tim_close(pdev);
}

int hal_tim_clear_update_flag(hal_tim_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_OK;
}

int hal_virtual_tim_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(arg);
    COMPAT_IGNORE_RESULT(irq_num);
    return VFS_ERR_NOTSUPP;
}
