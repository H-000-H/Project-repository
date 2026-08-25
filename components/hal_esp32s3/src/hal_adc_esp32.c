/**
 * SPDX-License-Identifier: Apache-2.0
 * @brief ESP32-S3 ADC HAL — adc_oneshot 后端（无 DMA）
 *
 * DTS 映射:
 *   adc-base / adc_handle → 1=ADC_UNIT_1, 2=ADC_UNIT_2
 *   channelN.channel_id   → adc_channel_t
 *   channelN.attenuation  → 0..3 → ADC_ATTEN_DB_*
 */
#include "hal_adc.h"
#include "status.h"
#include "compiler_compat.h"

#include "esp_adc/adc_oneshot.h"

#ifndef HAL_ADC_ESP_MAX
#define HAL_ADC_ESP_MAX  2
#endif

struct esp_adc_runtime
{
    int                        used;
    adc_oneshot_unit_handle_t  unit;
    adc_unit_t                 unit_id;
    uint32_t                   channel_count;
    adc_channel_t              channels[HAL_ADC_MAX_CHANNELS];
};

static struct esp_adc_runtime s_adc_rt[HAL_ADC_ESP_MAX];

static adc_atten_t esp_adc_map_atten(uint32_t atten)
{
    switch (atten)
    {
    case 0:  return ADC_ATTEN_DB_0;
    case 1:  return ADC_ATTEN_DB_2_5;
    case 2:  return ADC_ATTEN_DB_6;
    case 3:
    default: return ADC_ATTEN_DB_12;
    }
}

static struct esp_adc_runtime* esp_adc_rt_of(hal_adc_device* pdev)
{
    if (!pdev || !pdev->unique)
        return NULL;
    return (struct esp_adc_runtime*)pdev->unique->private_cfg;
}

int hal_adc_device_init(hal_adc_device* pdev, hal_adc_platform_unique_config* unique_cfg, hal_adc_host_config* host)
{
    int i;

    if (!pdev || !unique_cfg || !host)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->unique = unique_cfg;
    pdev->host   = host;

    for (i = 0; i < HAL_ADC_ESP_MAX; i++)
    {
        if (!s_adc_rt[i].used)
        {
            COMPAT_MEM_SET(&s_adc_rt[i], 0, sizeof(s_adc_rt[i]));
            s_adc_rt[i].used = 1;
            unique_cfg->private_cfg = (uintptr_t)&s_adc_rt[i];
            return VFS_OK;
        }
    }
    return VFS_ERR_NOMEM;
}

int hal_adc_device_deinit(hal_adc_device* pdev)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);

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

int hal_adc_init(hal_adc_device* pdev)
{
    struct esp_adc_runtime*     rt;
    adc_oneshot_unit_init_cfg_t unit_cfg;
    uint32_t                    i;

    if (!pdev || !pdev->host)
        return VFS_ERR_INVAL;

    rt = esp_adc_rt_of(pdev);
    if (!rt)
        return VFS_ERR_INVAL;

    rt->unit_id = (pdev->host->adc_handle == 2) ? ADC_UNIT_2 : ADC_UNIT_1;
    rt->channel_count = pdev->host->channel_count;
    if (rt->channel_count == 0 || rt->channel_count > HAL_ADC_MAX_CHANNELS)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(&unit_cfg, 0, sizeof(unit_cfg));
    unit_cfg.unit_id = rt->unit_id;
    if (adc_oneshot_new_unit(&unit_cfg, &rt->unit) != ESP_OK)
        return VFS_ERR_IO;

    for (i = 0; i < rt->channel_count; i++)
    {
        adc_oneshot_chan_cfg_t chan_cfg;
        adc_channel_t          ch;

        if (!pdev->host->channels)
        {
            COMPAT_IGNORE_RESULT(adc_oneshot_del_unit(rt->unit));
            rt->unit = NULL;
            return VFS_ERR_INVAL;
        }

        ch = (adc_channel_t)pdev->host->channels[i].channel_id;
        rt->channels[i] = ch;

        COMPAT_MEM_SET(&chan_cfg, 0, sizeof(chan_cfg));
        chan_cfg.atten = esp_adc_map_atten(pdev->host->channels[i].attenuation);
        chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(rt->unit, ch, &chan_cfg) != ESP_OK)
        {
            COMPAT_IGNORE_RESULT(adc_oneshot_del_unit(rt->unit));
            rt->unit = NULL;
            return VFS_ERR_IO;
        }
    }

    return VFS_OK;
}

int hal_adc_deinit_all_adcx(hal_adc_device* pdev)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);

    if (!rt)
        return VFS_ERR_INVAL;
    if (rt->unit)
    {
        COMPAT_IGNORE_RESULT(adc_oneshot_del_unit(rt->unit));
        rt->unit = NULL;
    }
    return VFS_OK;
}

int hal_adc_deinit_adcx_channel(hal_adc_device* pdev, uint32_t channel_id)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(channel_id);
    return VFS_OK;
}

int hal_adc_start(hal_adc_device* pdev)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);

    if (!rt || !rt->unit)
        return VFS_ERR_IO;
    return VFS_OK;
}

int hal_adc_stop(hal_adc_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_OK;
}

int hal_adc_read_value(hal_adc_device* pdev, uint32_t channel_num, uint16_t* out_val)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);
    int                     raw = 0;
    uint32_t                i;
    adc_channel_t           ch;

    if (!rt || !rt->unit || !out_val)
        return VFS_ERR_INVAL;

    ch = (adc_channel_t)channel_num;
    for (i = 0; i < rt->channel_count; i++)
    {
        if (rt->channels[i] == ch)
            break;
    }
    if (i >= rt->channel_count)
    {
        /* 允许直接按硬件通道号读 */
        ch = (adc_channel_t)channel_num;
    }

    if (adc_oneshot_read(rt->unit, ch, &raw) != ESP_OK)
        return VFS_ERR_IO;

    *out_val = (uint16_t)raw;
    return VFS_OK;
}

int hal_adc_poll_for_conversion(hal_adc_device* pdev, uint32_t* out_status)
{
    if (!out_status)
        return VFS_ERR_INVAL;
    COMPAT_IGNORE_RESULT(pdev);
    *out_status = 1;
    return VFS_OK;
}

int hal_adc_get_channel_count(hal_adc_device* pdev, uint32_t* count)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);

    if (!rt || !count)
        return VFS_ERR_INVAL;
    *count = rt->channel_count;
    return VFS_OK;
}

int hal_adc_get_channel_id(hal_adc_device* pdev, int index, uint32_t* channel_id)
{
    struct esp_adc_runtime* rt = esp_adc_rt_of(pdev);

    if (!rt || !channel_id || index < 0 || (uint32_t)index >= rt->channel_count)
        return VFS_ERR_INVAL;
    *channel_id = (uint32_t)rt->channels[index];
    return VFS_OK;
}

int hal_adc_get_channel_sample_time(hal_adc_device* pdev, int index, uint32_t* sample_time)
{
    if (!pdev || !pdev->host || !sample_time || !pdev->host->channels)
        return VFS_ERR_INVAL;
    if (index < 0 || (uint32_t)index >= pdev->host->channel_count)
        return VFS_ERR_INVAL;
    *sample_time = pdev->host->channels[index].sample_time;
    return VFS_OK;
}

int hal_adc_dma_start(hal_adc_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_ERR_NOTSUPP;
}

int hal_adc_dma_it_start(hal_adc_device* pdev)
{
    COMPAT_IGNORE_RESULT(pdev);
    return VFS_ERR_NOTSUPP;
}

int hal_adc_dma_it_read_value(hal_adc_device* pdev, uint16_t* out_val)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(out_val);
    return VFS_ERR_NOTSUPP;
}

int hal_adc_dma_read_value(hal_adc_device* pdev, uint16_t* out_val)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(out_val);
    return VFS_ERR_NOTSUPP;
}

int hal_virtual_adc_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(arg);
    COMPAT_IGNORE_RESULT(irq_num);
    return 0;
}

void hal_adc_dma_bottom_half_handler(void* arg)
{
    COMPAT_IGNORE_RESULT(arg);
}
