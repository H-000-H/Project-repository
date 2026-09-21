/* PC 输入: 窗口鼠标 → LVGL 指针输入设备的采样源 */
#ifndef PC_INPUT_H
#define PC_INPUT_H

#include "lvgl.h"

/** @brief 更新鼠标状态 (窗口消息里调, 主线程) */
void pc_input_mouse_update(int x, int y, bool pressed);

/**
 * @brief LVGL 指针采样回调: 把当前鼠标状态写进 data
 * @note  交给 ui::PointerInput 当采样源用 —— 建 indev、绑 display 都由 PointerInput 负责,
 *        这里只管"读当前鼠标状态"。LVGL 不线程安全, 本回调只在 UI 线程被调用。
 */
void pc_input_read_lvgl(lv_indev_data_t* data);

#endif /* PC_INPUT_H */
