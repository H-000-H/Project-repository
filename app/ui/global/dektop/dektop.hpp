/**
 * @file dektop.hpp
 * @author H-000-H
 * @brief  桌面(壁纸 + 顶层文字), 常存 static 组件; 继承骨架与 top-bar 一致
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DESKTOP_HPP
#define DESKTOP_HPP
#include "lvgl/lvgl.h"
#include <cstdint>
#include <lvgl/display/lv_display.h>
#include <lvgl/image/lv_image_decoder.h>
namespace ui
{
    #define DEKTOP_DEFAULT_FONT LV_FONT_DEFAULT_MONTSERRAT_12
    class DesktopViewModule
    {
    public:
        DesktopViewModule() = default;  /**< 默认构造 */
        ~DesktopViewModule() = default; /**< 默认析构 */
        /**
         * @brief  创建桌面: 壁纸 + 透明基准屏幕组件
         * @return 成功返回 true(1), 失败返回 false(0)
         */
        int  CreateDesktop();
        /**
         * @brief 设置壁纸图片
         * @param Image 壁纸图片描述符
         */
        void SetWallPaper(lv_image_dsc_t & Image);
        /**
         * @brief  创建顶层文字标签
         * @param  text 文字内容
         * @return 成功返回 true, 基准屏幕未创建返回 false
         */
        bool CreateTopText(const char* text);
        /**
         * @brief 更新顶层文字内容
         * @param text 新文字内容 (top_text 为空时忽略)
         */
        void SetTopText(const char* text);
        /**
         * @brief  获取桌面本身
         * @return 基准屏幕组件指针
         */
        lv_obj_t* GetScreen() const { return this->screen; }
        /**
         * @brief  获取顶层文字标签
         * @return 顶层文字标签指针 (未创建为 nullptr)
         */
        lv_obj_t* GetTopText(){return this->top_text;};
    protected:
        const uint32_t kDesktopWidth  = 320;
        const uint32_t kDesktopHeight = 240;
        lv_obj_t* screen   = nullptr;  /*顶层屏幕组件的基准屏幕(子页面都挂它下面)*/
        lv_obj_t* top_text = nullptr;  /*顶层屏幕组件的顶部文字*/
        lv_obj_t* image    = nullptr;  /*壁纸: 父组件是屏幕本身, 不是本类的 screen */
    };

    class DesktopDataModule
    {
    public:
        DesktopDataModule() = default;  /**< 默认构造 */
        ~DesktopDataModule() = default; /**< 默认析构 */
    };

    class DesktopControllerModule : public DesktopViewModule, public DesktopDataModule
    {
    public:
        DesktopControllerModule() = default;  /**< 默认构造 */
        ~DesktopControllerModule() = default; /**< 默认析构 */
    };

    class Desktop : public DesktopControllerModule
    {
    public:
        Desktop() = default;  /**< 默认构造 */
        ~Desktop() = default; /**< 默认析构 */
        Desktop(const Desktop&) = delete;            /**< 禁用拷贝构造 */
        Desktop& operator=(const Desktop&) = delete; /**< 禁用拷贝赋值 */
        Desktop(Desktop&&) = delete;                 /**< 禁用移动构造 */
        Desktop& operator=(Desktop&&) = delete;      /**< 禁用移动赋值 */
        /**
         * @brief  桌面是常驻的所以可以static 其他不是常驻可能需要析构所以不能static
         * @return 桌面单例引用
         */
        static Desktop& GetInstance(){static Desktop instance ; return instance;}
    };
}
#endif
