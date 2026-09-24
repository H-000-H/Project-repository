#ifndef SETTING_BASE_HPP
#define SETTING_BASE_HPP
/*setting界面的通用操作(避免每页都写一堆代码)*/
#include "lvgl.h"
#include "home.hpp"
#include <cstdint>
#include <etl/string.h>

namespace ui
{
    namespace setting_base
    {
        constexpr lv_color_t SettingDefaultColor = HomeColor::k_white;
        constexpr uint8_t    SettingDefaultTextSize = 32;
        /* 图片/箭头边长(占父对象高度百分比) */
        constexpr uint8_t    SettingIconHeightPct = 80;
        constexpr const lv_font_t* SettingFontSmall   = &lv_font_montserrat_12;
        constexpr const lv_font_t* SettingFontNormal  = &lv_font_montserrat_16;
        constexpr const lv_font_t* SettingDefaultFont = SettingFontSmall;

        /* 行内三个孩子的位置固定: 文字 / 图片 / 箭头(switch 无箭头) */
        constexpr uint8_t ChildText  = 0;
        constexpr uint8_t ChildImage = 1;
        constexpr uint8_t ChildIcon  = 2;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建选项
         * @param[in] parent lv_obj_t* 父对象
         * @param[in] text 选项文本(第1个孩子)
         * @param[in] width int 宽度
         * @param[in] height int 高度
         * @param[out] option_obj lv_obj_t** 选项对象
         * @param[out] text_obj lv_obj_t** 文本对象
         * @param[out] image_obj lv_obj_t** 图片对象(第2个孩子, 无图源时仅占位)
         * @param[out] icon_obj lv_obj_t** 箭头对象(第3个孩子)
         * @param[in] icon_text 箭头文本, 留空则用默认 LV_SYMBOL_RIGHT
         * @param[in] image_src const void* 图片源, 默认无(单片机无图源)
         * @return bool 是否成功
         * @note 
         *   - 三个孩子无论是否需要都会创建, 保证 child 序号固定
         */
        bool create_option( lv_obj_t* parent,
                            etl::string<SettingDefaultTextSize> text,
                            int width, int height,
                            lv_obj_t** option_obj,
                            lv_obj_t** text_obj,
                            lv_obj_t** image_obj = nullptr,
                            lv_obj_t** icon_obj = nullptr,
                            etl::string<SettingDefaultTextSize> icon_text = etl::string<SettingDefaultTextSize>(),
                            const void* image_src = nullptr);

        /**
         * @brief 动态设置选项颜色
         * @param[in] option_obj 选项对象
         * @param[in] color 选项颜色
         * @return 是否成功
         */
        bool set_option_color(lv_obj_t* option_obj, lv_color_t color);

/* --------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------------------------------------------------------------------------------- */
        /**
         * @brief 创建滚轮对象每个setting page 自己 调用去创建一个
         * @param[in] parent lv_obj_t* 挂载父对象 (page)
         * @return lv_obj_t* 滚动容器对象
         */
        lv_obj_t* create_scroll(lv_obj_t* parent);

        /**
         * @brief : 设置滑轮的位置
         * @param[in] parent lv_obj_t* 滚动容器对象
         * @param[in] x lv_coord_t 水平位置
         * @param[in] y lv_coord_t 垂直位置
         * @return bool 是否设置成功
         */
        bool set_scroll_postion(lv_obj_t* parent, lv_coord_t x, lv_coord_t y);

        bool set_scroll_color(lv_obj_t* scroll, lv_color_t color);
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
        lv_obj_t* create_widgets(lv_obj_t* parent,uint8_t hight,uint8_t width);

        bool set_widgets_size(lv_obj_t* widgets,int32_t hight,int32_t width);

        /**
         * @brief 动态设置底板(小部件容器)背景色
         * @param[in] widgets lv_obj_t* 底板对象
         * @param[in] color 背景色
         * @return bool 是否成功
         */
        bool set_widgets_color(lv_obj_t* widgets, lv_color_t color);

        /*不需要set postion按照flex 布局自动对齐*/
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建setting 的 页面 页面都一个鸟样所以也直接到基类了
         * @param[in] parent lv_obj_t* 父对象
         * @return lv_obj_t* 页面对象
         */
        lv_obj_t* create_page(lv_obj_t* parent);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 创建开关行
         * @param[in] parent lv_obj_t* 父对象
         * @param[in] width int32_t 行宽
         * @param[in] height int32_t 行高
         * @param[in] text 开关文本(第1个孩子)
         * @param[out] switch_obj lv_obj_t** 开关控件(第3个孩子)
         * @param[out] text_obj lv_obj_t** 文本对象
         * @param[out] image_obj lv_obj_t** 图片对象(第2个孩子, 无图源时仅占位)
         * @param[in] image_src const void* 图片源, 默认无
         * @param[in] color lv_color_t 文本颜色
         * @return lv_obj_t* 行容器(透明无边界)
         * @details ：
         *  - 轨道(主背景)色：HomeColor::k_white, 动态改用 set_switch_color
         *  - 指示器(底板)色：HomeColor::k_gray,  动态改用 set_switch_indicator_color
         *  - 旋钮(按钮)色  ：HomeColor::k_blue,  动态改用 set_switch_knob_color
         *  - 字体颜色默认  ：HomeColor::k_black
         * @note 
         *  - switch 行没有箭头, 行容器三个孩子是 文字 / 图片 / 开关
         */
        lv_obj_t* create_switch(lv_obj_t* parent,
                                int32_t width,int32_t height,
                                etl::string<SettingDefaultTextSize> text,
                                lv_obj_t** switch_obj = nullptr,
                                lv_obj_t** text_obj = nullptr,
                                lv_obj_t** image_obj = nullptr,
                                const void* image_src = nullptr,
                                lv_color_t color = HomeColor::k_black);

        /**
         * @brief 动态设置开关轨道(主背景)颜色
         * @param[in] switch_obj lv_obj_t* 开关对象
         * @param[in] color 颜色
         * @return bool 是否成功
         */
        bool set_switch_color(lv_obj_t* switch_obj, lv_color_t color);

        /**
         * @brief 动态设置开关指示器(底板)颜色
         * @param[in] switch_obj lv_obj_t* 开关对象
         * @param[in] color 颜色
         * @return bool 是否成功
         */
        bool set_switch_indicator_color(lv_obj_t* switch_obj, lv_color_t color);

        /**
         * @brief 动态设置开关旋钮(按钮)颜色
         * @param[in] switch_obj lv_obj_t* 开关对象
         * @param[in] color 颜色
         * @return bool 是否成功
         */
        bool set_switch_knob_color(lv_obj_t* switch_obj, lv_color_t color);

        /**
         * @brief 将开关切换到右侧
         * @param[in] obj lv_obj_t* 开关对象
         * @return bool 是否成功
         */
        bool switch_to_right(lv_obj_t* obj);

        /**
         * @brief 将开关切换到左侧
         * @param[in] obj lv_obj_t* 开关对象
         * @return bool 是否成功
         */
        bool switch_to_left(lv_obj_t* obj);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 动态设置文字内容(第1个孩子)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] text 文字内容
         * @return bool 是否成功
         */
        bool set_text(lv_obj_t* obj, etl::string<SettingDefaultTextSize> text);

        /**
         * @brief 动态设置文字颜色(第1个孩子)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] color 文字颜色
         * @return bool 是否成功
         */
        bool set_text_color(lv_obj_t* obj, lv_color_t color);

        /**
         * @brief 动态设置文字字体(第1个孩子)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] font const lv_font_t* 字体, 传 nullptr 回落到主题默认字体
         * @return bool 是否成功
         */
        bool set_text_font(lv_obj_t* obj, const lv_font_t* font);

        /**
         * @brief 动态设置图片源(第2个孩子)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] src const void* 图片源, 传 nullptr 清空
         * @return bool 是否成功
         */
        bool set_image_src(lv_obj_t* obj, const void* src);

        /**
         * @brief 动态设置箭头文本(第3个孩子按钮内的标签)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] text 箭头文本
         * @return bool 是否成功
         * @note 
         *   - switch 行没有箭头, 第3个孩子是开关本身, 调用返回 false
         */
        bool set_icon_text(lv_obj_t* obj, etl::string<SettingDefaultTextSize> text);

        /**
         * @brief 动态设置箭头颜色(第3个孩子按钮内的标签)
         * @param[in] obj lv_obj_t* 选项对象
         * @param[in] color 箭头颜色
         * @return bool 是否成功
         * @note 
         *  - switch 行没有箭头, 第3个孩子是开关本身, 调用返回 false
         */
        bool set_icon_color(lv_obj_t* obj, lv_color_t color);
    }

}

#endif // SETTING_BASE_HPP
