/**
 * @file dektop.hpp
 * @author H-000-H
 * @brief 桌面 (常驻外壳): 壁纸 + 透明场景根 + 顶层提示文字
 * @note  由 App 持有, 页面通过 get_screen() 往里挂控件树。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DEKTOP_HPP
#define DEKTOP_HPP
#include "lvgl/lvgl.h"

namespace ui
{
    // 桌面 (常驻外壳)
    class Desktop
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        Desktop()  = default;
        ~Desktop() = default;

        Desktop(const Desktop&)            = delete;
        Desktop& operator=(const Desktop&) = delete;
        Desktop(Desktop&&)                 = delete;
        Desktop& operator=(Desktop&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 建壁纸 + 透明场景根 (幂等)
         * @param[in] disp lv_display_t* 本桌面的 display
         * @return bool disp 为空或 LVGL 分配失败返回 false
         * @note  必须显式给 disp, 不从 lv_screen_active() 取: 那是"默认屏", 多屏时会挂到别的屏上
         */
        bool create(lv_display_t* disp);

        /**
         * @brief 换壁纸图源
         * @param[in] image const lv_image_dsc_t& 图片描述符 (LVGL 只存指针, 描述符必须常驻)
         */
        void set_wallpaper(const lv_image_dsc_t& image);

        /**
         * @brief 设置顶层提示文字 (标签首次调用时惰性创建)
         * @param[in] text const char* 新内容, 传空串即收起提示
         */
        void set_top_text(const char* text);

        // 场景根句柄: 页面控件树的父节点 (未 create 时为 nullptr)
        lv_obj_t* get_screen() const { return this->screen; }

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 建顶层提示标签 (set_top_text 内部惰性调用)
        bool create_top_label();

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* screen    = nullptr; // 透明场景根 (页面父节点)
        lv_obj_t* wallpaper = nullptr; // 壁纸 image (父节点是 display 的 active screen)
        lv_obj_t* top_text  = nullptr; // 顶层提示标签
    };
}

#endif // DEKTOP_HPP
