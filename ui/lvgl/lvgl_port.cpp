/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file lvgl_port.cpp
 * @author H-000-H
 * @brief LVGL(v9) 屏适配实现: 单例取实例 + 每帧把 area/px_map 捕获进刷屏 lambda
 * @details
 *   刷屏链路: LVGL → lvgl_port_flush_cb → get_instance().Flush(lambda)
 *                   → lambda 里 device_ioctl(DISPLAY_CMD_FLUSH) → 屏驱动
 *   每帧在回调里现场造一个捕获式 lambda: 本帧的 area/px_map 进捕获, 不动的上下文
 *   (device/超时)由 DisplayBase::Flush 补进参数。
 *   结尾必须 lv_display_flush_ready(), 否则 LVGL 认为这帧永远没刷完 → 界面卡死。
 * @note 绘制缓冲: 第一块用基类的 m_flush_buffer, 第二块用本类的 m_flush_buffer_secondary,
 *       是否参与由 LvglPort::kUseDoubleBuffer 编译期决定 (关掉时第二块容量退化为 1 字节,
 *       且 set_buffers 传 nullptr)。开双缓冲前先把 kFlushBufferMaxBytes 降下来:
 *       BSS 按数组容量算, 32768 x 2 = 64KB 在 128KB SRAM 上会挤掉别的东西。
 */
#include "lvgl_port.hpp"
#include "mini_time.h"
#include "display_drv.h"
#include "mini_time.h"
#include "status.h"
#include "system_log.h"
#include <cstddef>
#include <cstdint>

/** @brief 全局 lv_init() 只做一次的标记 (LVGL 运行时是全局的, 不是每个屏一份) */
static bool s_lvgl_inited = false;

/* 单例用的面板参数: 与 DTS 的 st7789 面板节点一致, 需要时用 -D 覆盖 */
#ifndef LVGL_PORT_PANEL_HEIGHT
#define LVGL_PORT_PANEL_HEIGHT 320U /**< 面板高 (行) */
#endif
#ifndef LVGL_PORT_PANEL_WIDTH
#define LVGL_PORT_PANEL_WIDTH 240U /**< 面板宽 (列) */
#endif
#ifndef LVGL_PORT_PANEL_BPP
#define LVGL_PORT_PANEL_BPP 16U /**< 像素位宽 (16 = RGB565) */
#endif
#ifndef LVGL_PORT_PANEL_LABEL
#define LVGL_PORT_PANEL_LABEL "st7789" /**< DTS label */
#endif
#ifndef LVGL_PORT_PANEL_OCCUPY
#define LVGL_PORT_PANEL_OCCUPY 10U /**< 整屏分几块刷 */
#endif

namespace display
{

LvglPort& LvglPort::get_instance()
{
    static LvglPort s_instance(LVGL_PORT_PANEL_HEIGHT, LVGL_PORT_PANEL_WIDTH, LVGL_PORT_PANEL_BPP,
                               LVGL_PORT_PANEL_LABEL, LVGL_PORT_PANEL_OCCUPY);
    return s_instance;
}

int LvglPort::Init()
{
    struct display_info_arg info;
    int                     ret;

    /* 幂等: 已建过 display 就直接返回 */
    if (disp != nullptr)
    {
        return MINI_OK;
    }
    if (m_device == nullptr)
    {
        MT_LOG_ERROR(kTag, "panel device is null (label 没查到, 构造时已报过)");
        return MINI_ERR_INVAL;
    }

    ret = device_open(m_device, NULL);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "panel open failed: %d", ret);
        return ret;
    }

    ret = device_ioctl(m_device, DISPLAY_CMD_GET_INFO, &info, sizeof(info), 0);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "GET_INFO failed: %d", ret);
        MINI_IGNORE_RESULT(device_close(m_device));
        return ret;
    }
    /* 本 port 只走高彩 RGB565; 单色屏请用 ui/u8g2 */
    if ((info.format != DISPLAY_FMT_RGB565) || (info.width == 0U) || (info.height == 0U))
    {
        MT_LOG_ERROR(kTag, "panel not usable: fmt=%u %ux%u", (unsigned)info.format, (unsigned)info.width,
                     (unsigned)info.height);
        MINI_IGNORE_RESULT(device_close(m_device));
        return MINI_ERR_NOTSUPP;
    }

    if (!s_lvgl_inited)
    {
        lv_init();
        lv_tick_set_cb(mini_time_ms);
        s_lvgl_inited = true;
    }

    disp = lv_display_create((int32_t)info.width, (int32_t)info.height);
    if (disp == nullptr)
    {
        MT_LOG_ERROR(kTag, "lv_display_create failed (LV_MEM_SIZE 不够?)");
        MINI_IGNORE_RESULT(device_close(m_device));
        return MINI_ERR_NOMEM;
    }

    /* st7789 的 RAMCTRL 配的是大端, LVGL 的 RGB565 在内存里是小端先交换 */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, lvgl_port_flush_cb);
    /* PARTIAL 分块刷: 长度用算出来的单块长度(受 occupy 控制) 关掉时必
     * 传 nullptr —— 传个 1 字节的缓冲会让 LVGL 按同样的长度往上写, 直接越界。 */
    lv_display_set_buffers(disp, m_flush_buffer.data(),
                           kUseDoubleBuffer ? m_flush_buffer_secondary.data() : nullptr,
                           (uint32_t)m_flush_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (lv_display_get_default() == nullptr)
    {
        lv_display_set_default(disp);
    }

    MT_LOG_INFO(kTag, "display ready: %ux%u fmt=%u buf=%uB", (unsigned)info.width, (unsigned)info.height,
                (unsigned)info.format, (unsigned)m_flush_buffer_size);
    return MINI_OK;
}

void LvglPort::lvgl_port_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    auto& instance = get_instance();

    instance.Flush([area, px_map](struct device* dev, std::size_t timeout) -> int
    {
        struct display_draw_arg arg = {0};
        int                     ret;

        arg.x      = static_cast<int16_t>(area->x1);
        arg.y      = static_cast<int16_t>(area->y1);
        arg.w      = static_cast<int16_t>(area->x2 - area->x1 + 1);
        arg.h      = static_cast<int16_t>(area->y2 - area->y1 + 1);
        arg.format = static_cast<uint8_t>(DISPLAY_FMT_RGB565);
        arg.data   = px_map;

        ret = device_ioctl(dev, DISPLAY_CMD_FLUSH, &arg, sizeof(arg), timeout);
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(kTag, "flush %d,%d..%d,%d failed: %d", static_cast<int>(area->x1), static_cast<int>(area->y1),
                         static_cast<int>(area->x2), static_cast<int>(area->y2), ret);
        }
        return ret;
    });
    lv_display_flush_ready(disp);
}

} // namespace display
