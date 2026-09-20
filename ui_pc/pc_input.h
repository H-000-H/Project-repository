/* PC 输入: 窗口鼠标 → LVGL pointer 输入设备 */
#ifndef PC_INPUT_H
#define PC_INPUT_H

/** @brief 更新鼠标状态 (窗口消息里调, 主线程) */
void pc_input_mouse_update(int x, int y, bool pressed);

/**
 * @brief 建 pointer indev 并挂上读回调
 * @note 必须在 UI 线程调用 (LVGL 不线程安全): 挂到"首帧刷屏回调"里最省事
 */
void pc_input_attach_lvgl(void);

#endif /* PC_INPUT_H */
