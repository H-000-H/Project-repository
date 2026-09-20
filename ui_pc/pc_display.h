/*
 * PC 侧的"面板": 一个 Win32 窗口 + 离屏 DIB, 代替嵌入式那边的 st7789 驱动。
 * UI 层照旧走 DISPLAY_CMD_GET_INFO / DISPLAY_CMD_FLUSH, 由 pc_port.cpp 转发到这里。
 */
#ifndef PC_DISPLAY_H
#define PC_DISPLAY_H

#include <cstdint>

/** @brief PC 面板尺寸 (代替 DTS 里的 st7789 240x320) */
inline constexpr int kPcPanelWidth  = 320;
inline constexpr int kPcPanelHeight = 240;

/** @brief 建窗口 + 离屏位图; hidden=true 时不显示窗口(只出图) */
void pc_display_open(bool hidden);

/** @brief 首帧刷屏时回调一次 (跑在 UI 线程): 建 LVGL 输入设备用 */
void pc_display_set_first_frame_hook(void (*hook)(void));

/** @brief 刷一块 RGB565 像素进窗口; byte_swapped = LVGL RGB565_SWAPPED 的字节序 */
void pc_display_blit(int x, int y, int w, int h, const uint16_t* px, bool byte_swapped);

/** @brief 跑消息循环; run_ms>0 则跑够时长自动退出; bmp_path 非空则退出前存一张 BMP */
void pc_display_pump(uint32_t run_ms, const char* bmp_path);

/** @brief 关窗口、释放资源 */
void pc_display_close(void);

#endif /* PC_DISPLAY_H */
