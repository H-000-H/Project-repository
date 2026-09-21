#include "pc_input.h"

namespace
{
    int  s_x       = 0;
    int  s_y       = 0;
    bool s_pressed = false;
} // namespace

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
