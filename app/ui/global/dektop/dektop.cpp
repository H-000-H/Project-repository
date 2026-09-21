/**
 * @file dektop.cpp
 * @author H-000-H
 * @brief 桌面外壳实现: 直接操作 LVGL 控件树, 无中间层
 * @note  接口说明见 dektop.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "dektop.hpp"
#include "home.hpp"
#include "system_log.h"
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_image_dsc.h>
#include <lvgl/widgets/lv_image.h>
#include <lvgl/widgets/lv_label.h>

LV_IMG_DECLARE(dektop_320x240);

namespace ui
{
    constexpr const char* const k_tag           = "desktop";
    constexpr const lv_font_t*  k_top_text_font = LV_FONT_DEFAULT_MONTSERRAT_12;

    bool Desktop::create(lv_display_t* disp)
    {
        if (this->screen != nullptr)
        {
            return true;
        }
        if (disp == nullptr)
        {
            MT_LOG_ERROR(k_tag, "create: disp 为空, 拒绝 (不用默认屏兜底, 多屏会挂错)");
            return false;
        }

        lv_obj_t* const scene = lv_display_get_screen_active(disp);

        this->wallpaper = lv_image_create(scene);
        if (this->wallpaper == nullptr)
        {
            MT_LOG_ERROR(k_tag, "wallpaper create failed");
            return false;
        }
        lv_image_set_src(this->wallpaper, &dektop_320x240);
        lv_obj_align(this->wallpaper, LV_ALIGN_TOP_LEFT, 0, 0);

        this->screen = lv_obj_create(scene);
        if (this->screen == nullptr)
        {
            MT_LOG_ERROR(k_tag, "screen create failed");
            return false;
        }
        lv_obj_set_size(this->screen, LV_PCT(100), LV_PCT(100));
        lv_obj_align(this->screen, LV_ALIGN_TOP_LEFT, 0, 0);
        /* 消除默认屏幕的边距与装饰, 只要一个透明容器 */
        lv_obj_set_style_pad_all(this->screen, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(this->screen, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(this->screen, LV_OPA_TRANSP, LV_PART_MAIN);
        return true;
    }

    void Desktop::set_wallpaper(const lv_image_dsc_t& image)
    {
        if (this->wallpaper != nullptr)
        {
            lv_image_set_src(this->wallpaper, &image);
        }
    }

    void Desktop::set_top_text(const char* text)
    {
        if (text == nullptr)
        {
            return;
        }
        if ((this->top_text == nullptr) && !this->create_top_label())
        {
            return;
        }
        /* lv_label_set_text 内部拷贝, 不吊在调用方指针上 */
        lv_label_set_text(this->top_text, text);
    }

    bool Desktop::create_top_label()
    {
        if (this->screen == nullptr)
        {
            MT_LOG_ERROR(k_tag, "screen 还没建, 先 create()");
            return false;
        }
        this->top_text = lv_label_create(this->screen);
        if (this->top_text == nullptr)
        {
            return false;
        }
        lv_obj_set_size(this->top_text, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_align(this->top_text, LV_ALIGN_TOP_MID, 0, LV_PCT(10));
        lv_obj_set_style_text_align(this->top_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(this->top_text, k_top_text_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(this->top_text, HomeColor::k_blue, LV_PART_MAIN);
        return true;
    }
}
