/**
 * @file pc_input.cpp
 * @author H-000-H
 * @brief PC 输入: 窗口鼠标状态 → LVGL 指针输入设备的采样源
 * @note  接口说明见 pc_input.h, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "pc_input.h"

namespace
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 最近一次鼠标状态 (窗口消息里写, UI 线程读)
    int  s_x       = 0;
    int  s_y       = 0;
    bool s_pressed = false;
} // namespace

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
void pc_input_mouse_update(int x, int y, bool pressed)
{
    s_x       = x;
    s_y       = y;
    s_pressed = pressed;
}

void pc_input_read_lvgl(lv_indev_data_t* data)
{
    if (data == nullptr)
    {
        return;
    }
    data->point.x = static_cast<lv_coord_t>(s_x);
    data->point.y = static_cast<lv_coord_t>(s_y);
    data->state   = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
