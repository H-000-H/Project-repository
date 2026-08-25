/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file lvgl_port.c
 *@brief LVGL v9 胶水层实现 — LVGL 显示对象经 VFS 屏设备 (display_drv.h) 落屏
 *@author H-000-H
 *@details
 *   数据流: 应用任务 → lvgl_port_task_handler → LVGL 渲染 → lvgl_port_flush_callback
 *           → display_lvgl_flush_callback → device_ioctl(DISPLAY_CMD_FLUSH) → 屏驱动 → bus → HAL。
 *   tick 源: osal_time_ms (lv_tick_set_cb); 上下文池静态分配, 渲染缓冲由调用方静态提供,
 *无动态内存。
 */

#include "lvgl_port.h"

#include "compiler_compat.h"
#include "display_drv.h"
#include "display_ui_bridge.h"
#include "osal.h"
#include "status.h"
#include "system_log.h"

static const char* const s_kTag = "lvgl_port";
static int s_lvgl_port_initialized;
static struct lvgl_display_context
    s_lvgl_display_context_pool[CONFIG_UI_LVGL_DISPLAY_COUNT] COMPAT_ALIGNED(4);

uint32_t lvgl_port_tick_millisecond(void) { return osal_time_ms(); }

int lvgl_port_init(void)
{
    if (s_lvgl_port_initialized)
        return MINI_OK;
    lv_init();
    lv_tick_set_cb(lvgl_port_tick_millisecond);
    s_lvgl_port_initialized = 1;
    SYS_LOGI(s_kTag, "init OK");
    return MINI_OK;
}

/**
 * @brief LVGL flush 回调：区域像素经 display_ui_bridge 落屏后回报 flush_ready
 * @param[in] lvgl_display LVGL 显示对象
 * @param[in] flush_area 刷新区域（含端点）
 * @param[in] pixel_map 渲染缓冲像素数据
 */
static void lvgl_port_flush_callback(lv_display_t* lvgl_display, const lv_area_t* flush_area,
                                     uint8_t* pixel_map)
{
    struct lvgl_display_context* display_context;

    display_context = (struct lvgl_display_context*)lv_display_get_user_data(lvgl_display);
    if (display_context && display_context->display_device)
    {
        COMPAT_IGNORE_RESULT(display_lvgl_flush_callback(
            display_context->display_device, (int16_t)flush_area->x1, (int16_t)flush_area->y1,
            (int16_t)flush_area->x2, (int16_t)flush_area->y2, pixel_map,
            display_context->pixel_format, CONFIG_UI_LVGL_FLUSH_TIMEOUT_MS));
    }
    else
    {
        SYS_LOGW(s_kTag, "flush without display device");
    }
    lv_display_flush_ready(lvgl_display);
}

struct lvgl_display_context* lvgl_display_attach(struct device* display_device,
                                                 uint8_t* primary_buffer, uint8_t* secondary_buffer,
                                                 uint32_t buffer_size_bytes)
{
    struct display_info_arg panel_info;
    struct lvgl_display_context* display_context;
    lv_display_t* lvgl_display;
    int slot_index;
    int ret;

    if (!display_device || !primary_buffer || buffer_size_bytes == 0U)
        return NULL;
    if (lvgl_port_init() != MINI_OK)
        return NULL;

    display_context = NULL;
    for (slot_index = 0; slot_index < CONFIG_UI_LVGL_DISPLAY_COUNT; slot_index++)
    {
        if (!s_lvgl_display_context_pool[slot_index].in_use)
        {
            display_context = &s_lvgl_display_context_pool[slot_index];
            break;
        }
    }
    if (!display_context)
    {
        SYS_LOGE(s_kTag, "no free display slot");
        return NULL;
    }

    ret = device_open(display_device, NULL);
    if (ret != MINI_OK)
    {
        SYS_LOGE(s_kTag, "device_open failed %d", ret);
        return NULL;
    }

    ret = device_ioctl(display_device, DISPLAY_CMD_GET_INFO, &panel_info, sizeof(panel_info),
                       CONFIG_UI_LVGL_FLUSH_TIMEOUT_MS);
    if (ret != MINI_OK)
    {
        SYS_LOGE(s_kTag, "GET_INFO failed %d", ret);
        COMPAT_IGNORE_RESULT(device_close(display_device));
        return NULL;
    }

    lvgl_display = lv_display_create((int32_t)panel_info.width, (int32_t)panel_info.height);
    if (!lvgl_display)
    {
        SYS_LOGE(s_kTag, "lv_display_create failed");
        COMPAT_IGNORE_RESULT(device_close(display_device));
        return NULL;
    }

    display_context->display_device = display_device;
    display_context->lvgl_display = lvgl_display;
    display_context->pixel_format = panel_info.format;
    display_context->in_use = 1;
    lv_display_set_user_data(lvgl_display, display_context);
    lv_display_set_buffers(lvgl_display, primary_buffer, secondary_buffer, buffer_size_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lvgl_display, lvgl_port_flush_callback);
    SYS_LOGI(s_kTag, "attach %ux%u fmt=%u", panel_info.width, panel_info.height, panel_info.format);
    return display_context;
}

int lvgl_display_detach(struct lvgl_display_context* display_context)
{
    int ret;

    if (!display_context || !display_context->in_use)
        return MINI_ERR_INVAL;
    if (display_context->lvgl_display)
        lv_display_delete(display_context->lvgl_display);
    ret = MINI_OK;
    if (display_context->display_device)
        ret = device_close(display_context->display_device);
    display_context->display_device = NULL;
    display_context->lvgl_display = NULL;
    display_context->pixel_format = 0U;
    display_context->in_use = 0;
    return ret;
}

uint32_t lvgl_port_task_handler(void)
{
    if (!s_lvgl_port_initialized)
        return 0U;
    return lv_timer_handler();
}
