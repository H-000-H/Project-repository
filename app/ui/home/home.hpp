/**
 * @file home.hpp
 * @author H-000-H
 * @brief 常用数据定义 —— 颜色表
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef HOME_HPP
#define HOME_HPP
#include <lvgl/lvgl.h>
namespace  ui
{
    /* 颜色表: 每个成员的类型直接就是 lv_color_t, 拿到就能用, 不用再转。
     *
     * 以前这里是 24 位色号枚举(0xFF0000 这种), 调用方得自己转; 有人把它喂给 lv_color_hex3(),
     * 而那个函数只认 12 位 0xRGB(内部只取低 12 位), 于是红(0xFF0000) 取低 12 位 = 0x000 → 变纯黑。
     * 现在成员本身就是颜色, 再误传给 lv_color_hex3() 会直接编译不过, 这个坑就没了。
     *
     * LV_COLOR_MAKE(r8, g8, b8) 展开是聚合初始化 {blue, green, red}, 是编译期常量:
     * static constexpr 直接进只读段, 不产生启动期初始化代码。
     */
    struct HomeColor
    {
        static constexpr lv_color_t White        = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
        static constexpr lv_color_t Black        = LV_COLOR_MAKE(0x00, 0x00, 0x00);
        static constexpr lv_color_t Red          = LV_COLOR_MAKE(0xFF, 0x00, 0x00);
        static constexpr lv_color_t Green        = LV_COLOR_MAKE(0x00, 0xFF, 0x00);
        static constexpr lv_color_t Blue         = LV_COLOR_MAKE(0x00, 0x00, 0xFF);
        static constexpr lv_color_t Yellow       = LV_COLOR_MAKE(0xFF, 0xFF, 0x00);
        static constexpr lv_color_t Cyan         = LV_COLOR_MAKE(0x00, 0xFF, 0xFF);
        static constexpr lv_color_t Magenta      = LV_COLOR_MAKE(0xFF, 0x00, 0xFF);
        static constexpr lv_color_t Gray         = LV_COLOR_MAKE(0x80, 0x80, 0x80);
        static constexpr lv_color_t Grey         = LV_COLOR_MAKE(0x80, 0x80, 0x80);
        static constexpr lv_color_t Brown        = LV_COLOR_MAKE(0xA5, 0x2A, 0x2A);
        static constexpr lv_color_t Orange       = LV_COLOR_MAKE(0xFF, 0xA5, 0x00);
        static constexpr lv_color_t Pink         = LV_COLOR_MAKE(0xFF, 0xC0, 0xCB);
        static constexpr lv_color_t Purple       = LV_COLOR_MAKE(0x80, 0x00, 0x80);
        static constexpr lv_color_t Teal         = LV_COLOR_MAKE(0x00, 0x80, 0x80);
        static constexpr lv_color_t Silver       = LV_COLOR_MAKE(0xC0, 0xC0, 0xC0);
        static constexpr lv_color_t Gold         = LV_COLOR_MAKE(0xFF, 0xD7, 0x00);
        static constexpr lv_color_t Bronze       = LV_COLOR_MAKE(0xCD, 0x7F, 0x32);
        static constexpr lv_color_t Rose         = LV_COLOR_MAKE(0xFF, 0x00, 0xFF);
        static constexpr lv_color_t Emerald      = LV_COLOR_MAKE(0x50, 0xC8, 0x78);
        static constexpr lv_color_t Sapphire     = LV_COLOR_MAKE(0x34, 0x98, 0xDB);
        static constexpr lv_color_t Ruby         = LV_COLOR_MAKE(0xE0, 0x11, 0x5F);
        static constexpr lv_color_t EmeraldGreen = LV_COLOR_MAKE(0x50, 0xC8, 0x78);
        static constexpr lv_color_t RoyalBlue    = LV_COLOR_MAKE(0x41, 0x69, 0xE1);
        static constexpr lv_color_t Tanzanite    = LV_COLOR_MAKE(0x00, 0x18, 0x8F);
        static constexpr lv_color_t Amethyst     = LV_COLOR_MAKE(0x99, 0x66, 0xFF);
    };
}
#endif
