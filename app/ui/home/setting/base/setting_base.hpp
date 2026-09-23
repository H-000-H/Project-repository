#pragma once
/*setting界面的通用操作(避免每页都写一堆代码)*/
#include "etl/inplace_function.h"
#include "lvgl.h"
#include "system_log.h"
#include "home.hpp"
#include <cstdint>
#include <lvgl/api_map/lv_api_map_v8.h>
#include <lvgl/config/lv_conf_internal.h>
#include <lvgl/core/lv_area.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/lv_types.h>
#include <lvgl/widgets/lv_button.h>
#include <psdk_inc/_ip_types.h>
#include <etl/string.h>

namespace ui
{
    namespace setting_base
    {
        constexpr lv_color_t SettingDefaultColor = HomeColor::k_white;
        constexpr uint8_t    SettingDefaultTextSize = 32;
        #define SettingDefaultFont LV_FONT_MONTSERRAT_10

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建选项
         * @param[in] parent lv_obj_t* 父对象
         * @param[in] text const char* 选项文本
         * @param[in] width int 宽度
         * @param[in] height int 高度
         * @param[out] option_obj lv_obj_t** 选项对象
         * @param[out] text_obj lv_obj_t** 文本对象
         * @return bool 是否成功
         * //TODO
         */
        bool create_option(lv_obj_t* parent,etl::string<SettingDefaultTextSize> text,int width, int height,lv_obj_t** option_obj,lv_obj_t** text_obj)
        {
            /*防止多次初始化和空指针 */
            if(!parent||option_obj||text_obj)
                return false;
            *option_obj = lv_button_create(parent);
            if (option_obj == nullptr)
            {
                return false;
            }
            lv_obj_set_size(*option_obj,width,height);
            lv_obj_set_layout(*option_obj, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(*option_obj, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_bg_color(*option_obj, SettingDefaultColor, LV_PART_MAIN);
            lv_obj_set_style_radius(*option_obj,8, LV_PART_MAIN);
            lv_obj_set_style_opa(*option_obj,LV_PCT(90),LV_PART_MAIN);
            *text_obj = lv_label_create(*option_obj);
            if (text_obj == nullptr)
            {
                return false;
            }
            lv_label_set_text(*text_obj, text.c_str());
            lv_obj_set_style_text_align(*text_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(*text_obj,HomeColor::k_black, LV_PART_MAIN);
            lv_obj_set_style_text_font(*text_obj, SettingDefaultFont, LV_PART_MAIN);

            return true;
        }

        /**
         * @brief 动态设置选项文字和颜色 主要是文字所以颜色给了个默认值
         * @param[in] option_obj 选项对象
         * @param[in] text 文字内容
         * @param[in] color 文字颜色 默认白色
         * @return 是否成功
         */
        bool set_option_text(lv_obj_t* option_obj, etl::string<SettingDefaultTextSize> text, lv_color_t color = SettingDefaultColor)
        {
            if (!option_obj)
            {
                return false;
            }

            lv_label_set_text(option_obj, text.c_str());
            lv_obj_set_style_text_color(option_obj, color, LV_PART_MAIN);
            return true;
        }

        /**
         * @brief 动态设置选项颜色
         * @param[in] option_obj 选项对象
         * @param[in] color 选项颜色
         * @return 是否成功
         */
        bool set_option_color(lv_obj_t* option_obj, lv_color_t color)
        {
            if (!option_obj)
            {
                return false;
            }

            lv_obj_set_style_text_color(option_obj, color, LV_PART_MAIN);
            return true;
        }

/* --------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------------------------------------------------------------------------------- */
        /**
         * @brief 创建滚轮对象每个setting page 自己 调用去创建一个
         * @param[in] parent lv_obj_t* 挂载父对象 (page)
         * @return lv_obj_t* 滚动容器对象
         */
        lv_obj_t* create_scroll(lv_obj_t* parent)
        {
            lv_obj_t*scroll = lv_obj_create(parent);
            if (scroll == nullptr)
            {
                return nullptr;
            }
            lv_obj_set_scrollable(parent, true);
            lv_obj_set_style_bg_color(scroll, SettingDefaultColor, LV_PART_SCROLLBAR);
            lv_obj_set_style_size(scroll, LV_PCT(1), LV_PCT(90), LV_PART_SCROLLBAR);
            lv_obj_set_style_radius(scroll, 6, LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_opa(scroll, LV_OPA_20, LV_PART_SCROLLBAR);
            lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
            lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
            return scroll;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建小部件 默认正方形
         * @param[in] parent lv_obj_t* 挂载父对象 (page)
         * @param[in] ktag const char* 日志标签
         * @param[in] hight uint8_t 高度(比例)
         * @param[in] width uint8_t 宽度(比例)
         * @return lv_obj_t* 小部件对象
         */
        lv_obj_t* create_widgets(lv_obj_t* parent,uint8_t hight,uint8_t width)
        {
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
            lv_obj_set_style_radius(obj,0, LV_PART_MAIN);
            return obj;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建setting 的 页面 页面都一个鸟样所以也直接到基类了
         * @param[in] parent lv_obj_t* 父对象
         * @return lv_obj_t* 页面对象
         */
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
            lv_obj_set_size(page,LV_PCT(100),LV_PCT(100));
            lv_obj_set_style_bg_color(page,SettingDefaultColor, LV_PART_MAIN);
            lv_obj_set_layout(page,LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(page,LV_FLEX_FLOW_COLUMN);
            /*设置选项块的间距(选项是组件每个同类型的选项占一个选项块) */
            lv_obj_set_style_pad_row(page,LV_PCT(5),LV_PART_MAIN);

            lv_obj_set_style_pad_all(page,LV_PCT(5),LV_PART_MAIN);

            lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            return page;
        }
    }

}
