#include "pc_input.h"

#include <cstdio>

#include "lvgl.h"

namespace
{
    lv_indev_t* s_indev   = nullptr;
    int         s_x       = 0;
    int         s_y       = 0;
    bool        s_pressed = false;

    /** @brief LVGL 读回调: 每周期把当前鼠标点/按下状态报给 LVGL */
    void mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
    {
        (void)indev;
        data->point.x = static_cast<lv_coord_t>(s_x);
        data->point.y = static_cast<lv_coord_t>(s_y);
        data->state   = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
} // namespace

void pc_input_mouse_update(int x, int y, bool pressed)
{
    s_x       = x;
    s_y       = y;
    s_pressed = pressed;
}

void pc_input_attach_lvgl(void)
{
    if (s_indev != nullptr)
    {
        return;
    }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, mouse_read_cb);
    /* 鼠标不需要焦点组: 点谁谁得焦点 (靠 CLICK_FOCUSABLE 标志) */
    std::printf("[pc_input] mouse pointer indev attached\n");
}
