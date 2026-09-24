/**
 * @file setting_base.cpp
 * @author H-000-H
 * @brief setting 界面通用操作实现
 * @note  接口说明见 setting_base.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "setting_base.hpp"
#include "home.hpp"
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/font/lv_symbol_def.h>
#include <lvgl/font/lv_text.h>
#include <lvgl/widgets/lv_image.h>
#include <lvgl/widgets/lv_label.h>

namespace ui
{
    namespace setting_base
    {
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        namespace
        {
            /**
             * @brief 建"行容器(画布)": 透明无边界 pad 0, 只负责 flex row 排布
             * @param[in] parent lv_obj_t* 父对象
             * @param[in] width  int32_t   行宽
             * @param[in] height int32_t   行高
             * @return lv_obj_t* 行容器; 分配失败返回 nullptr
             */
            lv_obj_t* create_row_container(lv_obj_t* parent, int32_t width, int32_t height)
            {
                lv_obj_t* row = lv_obj_create(parent);
                if (row == nullptr)
                {
                    return nullptr;
                }
                lv_obj_set_size(row, width, height);
                lv_obj_set_layout(row, LV_LAYOUT_FLEX);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
                lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
                return row;
            }
        } // namespace

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        bool create_option(lv_obj_t* parent,
                           etl::string<SettingDefaultTextSize> text,
                           int width, int height,
                           lv_obj_t** option_obj,
                           lv_obj_t** text_obj,
                           lv_obj_t** image_obj,
                           lv_obj_t** icon_obj,
                           etl::string<SettingDefaultTextSize> icon_text,
                           const void* image_src)
        {
            // 防止多次初始化和空指针
            if (!parent || !option_obj || !text_obj)
            {
                return false;
            }
            lv_coord_t icon_side = (height * SettingIconHeightPct) / 100;
            *option_obj          = create_row_container(parent, width, height);
            if (*option_obj == nullptr)
            {
                return false;
            }

            // 孩子1: 文字, 吃掉剩余空间, 超长转为 ...
            *text_obj = lv_label_create(*option_obj);
            if (*text_obj == nullptr)
            {
                return false;
            }
            lv_label_set_text(*text_obj, text.c_str());
            lv_label_set_long_mode(*text_obj, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(*text_obj, 1);
            lv_obj_set_style_text_align(*text_obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_style_text_color(*text_obj, HomeColor::k_black, LV_PART_MAIN);
            lv_obj_set_style_text_font(*text_obj, SettingDefaultFont, LV_PART_MAIN);

            // 孩子2: 图片, 无图源时仅占位以保证孩子序号固定
            lv_obj_t* image = lv_image_create(*option_obj);
            if (image == nullptr)
            {
                return false;
            }
            lv_obj_set_size(image, icon_side, icon_side);
            if (image_src)
            {
                lv_image_set_src(image, image_src);
            }
            if (image_obj)
            {
                *image_obj = image;
            }

            // 孩子3: 按钮(内含箭头)
            lv_obj_t* button = lv_button_create(*option_obj);
            if (button == nullptr)
            {
                return false;
            }
            lv_obj_set_size(button, icon_side, icon_side);
            if (icon_obj)
            {
                *icon_obj = button;
            }

            lv_obj_t* icon = lv_label_create(button);
            if (icon == nullptr)
            {
                return false;
            }
            lv_label_set_text(icon, icon_text.empty() ? LV_SYMBOL_RIGHT : icon_text.c_str());
            lv_obj_center(icon);
            lv_obj_set_style_text_color(icon, HomeColor::k_black, LV_PART_MAIN);
            lv_obj_set_style_text_font(icon, SettingDefaultFont, LV_PART_MAIN);

            return true;
        }

        bool set_option_color(lv_obj_t* option_obj, lv_color_t color)
        {
            if (!option_obj)
            {
                return false;
            }

            lv_obj_set_style_bg_color(option_obj, color, LV_PART_MAIN);
            return true;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* create_scroll(lv_obj_t* parent)
        {
            if (parent == nullptr)
            {
                return nullptr;
            }
            lv_obj_t* scroll = lv_obj_create(parent);
            if (scroll == nullptr)
            {
                return nullptr;
            }
            lv_obj_set_scrollable(scroll, true);
            lv_obj_set_style_bg_color(scroll, SettingDefaultColor, LV_PART_SCROLLBAR);
            lv_obj_align(scroll, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_radius(scroll, 6, LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_opa(scroll, LV_OPA_20, LV_PART_SCROLLBAR);
            lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
            lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
            return scroll;
        }

        bool set_scroll_postion(lv_obj_t* parent, lv_coord_t x, lv_coord_t y)
        {
            if (parent == nullptr)
            {
                return false;
            }
            lv_obj_set_pos(parent, x, y);
            return true;
        }

        bool set_scroll_color(lv_obj_t* scroll, lv_color_t color)
        {
            if (scroll == nullptr)
            {
                return false;
            }
            lv_obj_set_style_bg_color(scroll, color, LV_PART_SCROLLBAR);
            return true;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* create_widgets(lv_obj_t* parent, uint8_t hight, uint8_t width)
        {
            if (parent == nullptr)
            {
                return nullptr;
            }
            lv_obj_t* obj = lv_obj_create(parent);
            if (obj == nullptr)
            {
                return nullptr;
            }
            lv_obj_set_size(obj, LV_PCT(width), LV_PCT(hight));
            lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
            lv_obj_set_style_bg_color(obj, SettingDefaultColor, LV_PART_MAIN);
            lv_obj_set_style_pad_all(obj, LV_PCT(2), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(obj, LV_OPA_90, LV_PART_MAIN);
            lv_obj_set_style_pad_column(obj, LV_PCT(4), LV_PART_MAIN);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
            return obj;
        }

        bool set_widgets_size(lv_obj_t* widgets, int32_t hight, int32_t width)
        {
            if (!widgets)
            {
                return false;
            }
            lv_obj_set_size(widgets, width, hight);
            return true;
        }

        bool set_widgets_color(lv_obj_t* widgets, lv_color_t color)
        {
            if (!widgets)
            {
                return false;
            }
            lv_obj_set_style_bg_color(widgets, color, LV_PART_MAIN);
            return true;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* create_page(lv_obj_t* parent)
        {
            if (!parent)
            {
                return nullptr;
            }
            lv_obj_t* page = lv_obj_create(parent);
            if (!page)
            {
                return nullptr;
            }
            lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_bg_color(page, SettingDefaultColor, LV_PART_MAIN);
            lv_obj_set_layout(page, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
            // 设置选项块的间距(选项是组件每个同类型的选项占一个选项块)
            lv_obj_set_style_pad_row(page, LV_PCT(5), LV_PART_MAIN);
            lv_obj_set_style_pad_all(page, LV_PCT(5), LV_PART_MAIN);
            lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            return page;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* create_switch(lv_obj_t* parent,
                                int32_t width, int32_t height,
                                etl::string<SettingDefaultTextSize> text,
                                lv_obj_t** switch_obj,
                                lv_obj_t** text_obj,
                                lv_obj_t** image_obj,
                                const void* image_src,
                                lv_color_t color)
        {
            if (!parent)
            {
                return nullptr;
            }
            lv_coord_t icon_side = (height * SettingIconHeightPct) / 100;

            lv_obj_t* row = create_row_container(parent, width, height);
            if (row == nullptr)
            {
                return nullptr;
            }

            // 孩子1: 文字, 吃掉剩余空间, 超长转为 ...
            lv_obj_t* label = lv_label_create(row);
            if (label == nullptr)
            {
                return nullptr;
            }
            lv_label_set_text(label, text.c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(label, 1);
            lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
            lv_obj_set_style_text_font(label, SettingDefaultFont, LV_PART_MAIN);
            if (text_obj)
            {
                *text_obj = label;
            }

            // 孩子2: 图片, 无图源时仅占位以保证孩子序号固定
            lv_obj_t* image = lv_image_create(row);
            if (image == nullptr)
            {
                return nullptr;
            }
            lv_obj_set_size(image, icon_side, icon_side);
            if (image_src)
            {
                lv_image_set_src(image, image_src);
            }
            if (image_obj)
            {
                *image_obj = image;
            }

            // 孩子3: 开关
            lv_obj_t* sw = lv_switch_create(row);
            if (sw == nullptr)
            {
                return nullptr;
            }
            lv_obj_set_size(sw, icon_side * 2, icon_side);

            // 轨道(主背景)
            lv_obj_set_style_bg_color(sw, SettingDefaultColor, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);

            // 指示器(底板)
            lv_obj_set_style_bg_color(sw, HomeColor::k_gray, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);

            // 旋钮(按钮)
            lv_obj_set_style_bg_color(sw, HomeColor::k_blue, LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);

            lv_obj_set_style_radius(sw, 8, LV_PART_MAIN);
            if (switch_obj)
            {
                *switch_obj = sw;
            }

            return row;
        }

        bool set_switch_color(lv_obj_t* switch_obj, lv_color_t color)
        {
            if (!switch_obj)
            {
                return false;
            }
            lv_obj_set_style_bg_color(switch_obj, color, LV_PART_MAIN);
            return true;
        }

        bool set_switch_indicator_color(lv_obj_t* switch_obj, lv_color_t color)
        {
            if (!switch_obj)
            {
                return false;
            }
            lv_obj_set_style_bg_color(switch_obj, color, LV_PART_INDICATOR);
            return true;
        }

        bool set_switch_knob_color(lv_obj_t* switch_obj, lv_color_t color)
        {
            if (!switch_obj)
            {
                return false;
            }
            lv_obj_set_style_bg_color(switch_obj, color, LV_PART_KNOB);
            return true;
        }

        bool switch_to_right(lv_obj_t* obj)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_add_state(obj, LV_STATE_CHECKED);
            return true;
        }

        bool switch_to_left(lv_obj_t* obj)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_remove_state(obj, LV_STATE_CHECKED);
            return true;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        bool set_text(lv_obj_t* obj, etl::string<SettingDefaultTextSize> text)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* text_obj = lv_obj_get_child(obj, ChildText);
            if (!text_obj)
            {
                return false;
            }
            lv_label_set_text(text_obj, text.c_str());
            return true;
        }

        bool set_text_color(lv_obj_t* obj, lv_color_t color)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* text_obj = lv_obj_get_child(obj, ChildText);
            if (!text_obj)
            {
                return false;
            }
            lv_obj_set_style_text_color(text_obj, color, LV_PART_MAIN);
            return true;
        }

        bool set_text_font(lv_obj_t* obj, const lv_font_t* font)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* text_obj = lv_obj_get_child(obj, ChildText);
            if (!text_obj)
            {
                return false;
            }
            lv_obj_set_style_text_font(text_obj, font, LV_PART_MAIN);
            return true;
        }

        bool set_image_src(lv_obj_t* obj, const void* src)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* image_obj = lv_obj_get_child(obj, ChildImage);
            if (!image_obj)
            {
                return false;
            }
            lv_image_set_src(image_obj, src);
            return true;
        }

        bool set_icon_text(lv_obj_t* obj, etl::string<SettingDefaultTextSize> text)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* icon_obj = lv_obj_get_child(obj, ChildIcon);
            if (!icon_obj)
            {
                return false;
            }
            lv_obj_t* label = lv_obj_get_child(icon_obj, 0);
            if (!label)
            {
                return false;
            }
            lv_label_set_text(label, text.c_str());
            return true;
        }

        bool set_icon_color(lv_obj_t* obj, lv_color_t color)
        {
            if (!obj)
            {
                return false;
            }
            lv_obj_t* icon_obj = lv_obj_get_child(obj, ChildIcon);
            if (!icon_obj)
            {
                return false;
            }
            lv_obj_t* label = lv_obj_get_child(icon_obj, 0);
            if (!label)
            {
                return false;
            }
            lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
            return true;
        }
    } // namespace setting_base

} // namespace ui
