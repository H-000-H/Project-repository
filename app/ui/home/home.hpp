/**
 * @file home.hpp
 * @brief 常用数据定义 —— 颜色表
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef HOME_HPP
#define HOME_HPP
#include <lvgl/lvgl.h>
namespace ui
{
    /**
     * @brief 颜色表: 成员类型直接是 lv_color_t, 拿到就能用
     * @note  以前这里是 24 位色号枚举(0xFF0000), 调用方自己转; 有人喂给 lv_color_hex3(),
     *        而那个只认 12 位 0xRGB(取低 12 位), 于是红色变纯黑。现在成员本身就是颜色,
     *        再误传给 lv_color_hex3() 会直接编译不过。
     */
    struct HomeColor
    {
        static constexpr lv_color_t k_white         = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
        static constexpr lv_color_t k_black         = LV_COLOR_MAKE(0x00, 0x00, 0x00);
        static constexpr lv_color_t k_red           = LV_COLOR_MAKE(0xFF, 0x00, 0x00);
        static constexpr lv_color_t k_green         = LV_COLOR_MAKE(0x00, 0xFF, 0x00);
        static constexpr lv_color_t k_blue          = LV_COLOR_MAKE(0x00, 0x00, 0xFF);
        static constexpr lv_color_t k_yellow        = LV_COLOR_MAKE(0xFF, 0xFF, 0x00);
        static constexpr lv_color_t k_cyan          = LV_COLOR_MAKE(0x00, 0xFF, 0xFF);
        static constexpr lv_color_t k_magenta       = LV_COLOR_MAKE(0xFF, 0x00, 0xFF);
        static constexpr lv_color_t k_gray          = LV_COLOR_MAKE(0x80, 0x80, 0x80);
        static constexpr lv_color_t k_brown         = LV_COLOR_MAKE(0xA5, 0x2A, 0x2A);
        static constexpr lv_color_t k_orange        = LV_COLOR_MAKE(0xFF, 0xA5, 0x00);
        static constexpr lv_color_t k_pink          = LV_COLOR_MAKE(0xFF, 0xC0, 0xCB);
        static constexpr lv_color_t k_purple        = LV_COLOR_MAKE(0x80, 0x00, 0x80);
        static constexpr lv_color_t k_teal          = LV_COLOR_MAKE(0x00, 0x80, 0x80);
        static constexpr lv_color_t k_silver        = LV_COLOR_MAKE(0xC0, 0xC0, 0xC0);
        static constexpr lv_color_t k_gold          = LV_COLOR_MAKE(0xFF, 0xD7, 0x00);
        static constexpr lv_color_t k_bronze        = LV_COLOR_MAKE(0xCD, 0x7F, 0x32);
        static constexpr lv_color_t k_rose          = LV_COLOR_MAKE(0xFF, 0x00, 0xFF);
        static constexpr lv_color_t k_emerald       = LV_COLOR_MAKE(0x50, 0xC8, 0x78);
        static constexpr lv_color_t k_sapphire      = LV_COLOR_MAKE(0x34, 0x98, 0xDB);
        static constexpr lv_color_t k_ruby          = LV_COLOR_MAKE(0xE0, 0x11, 0x5F);
        static constexpr lv_color_t k_emerald_green = LV_COLOR_MAKE(0x50, 0xC8, 0x78);
        static constexpr lv_color_t k_royal_blue    = LV_COLOR_MAKE(0x41, 0x69, 0xE1);
        static constexpr lv_color_t k_tanzanite     = LV_COLOR_MAKE(0x00, 0x18, 0x8F);
        static constexpr lv_color_t k_amethyst      = LV_COLOR_MAKE(0x99, 0x66, 0xFF);
    };
}
#endif
