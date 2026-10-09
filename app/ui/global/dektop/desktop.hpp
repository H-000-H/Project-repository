/**
 * @file desktop.hpp
 * @author H-000-H
 * @brief 桌面 (常驻外壳): 壁纸 + 透明场景根 + 顶层提示文字
 * @note  由 App 持有, 页面通过 get_screen() 往里挂控件树。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DESKTOP_HPP
#define DESKTOP_HPP
#include "etl/array.h"
#include "lvgl/lvgl.h"
#include <lvgl/lv_types.h>
#include "etl/string.h"
namespace ui
{
    class App; // 前向声明: 图标点击要跳页, 但 Desktop 是 App 的成员, 构造期拿不到它

    // 图标槽位上限 (栅格行数按它算)
    constexpr uint8_t k_app_bar_max_count = 10;
    // 栅格列数 (必须与 desktop.cpp 的列模板数组一致)
    constexpr uint8_t k_app_bar_columns = 4;

    // 桌面图标: 一格 = 可点区域 + 图标(位图或字形, 二选一) + 名字
    struct AppBar
    {
        lv_obj_t*   cell  = nullptr; /**< 格子 (点击事件挂它身上) */
        lv_obj_t*   image = nullptr; /**< 位图图标; 与 icon 二选一, 没选它时恒 nullptr */
        lv_obj_t*   icon  = nullptr; /**< 字形图标 (LV_SYMBOL_*); 与 image 二选一 */
        lv_obj_t*   name  = nullptr; /**< 名字标签 */
        const char* text  = nullptr; /**< 名字 (常驻字符串, 只存指针) */
        const char* page  = nullptr; /**< 点击跳的页注册名 (常驻; nullptr = 不响应) */
    };

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

        /**
         * @brief 注入换页入口
         * @param[in] app App& 组合根
         * @note  Desktop 是 App 的成员, 构造期 App 还没成形, 只能由 App::init 事后注入
         */
        void bind_router(App& app) { this->router = &app; }

        /**
         * @brief 往桌面栅格里加一格图标
         * @param[in] icon const char* 字形图标 (LV_SYMBOL_*, 常驻); 与 image_src 二选一
         * @param[in] text const char* 图标名字 (常驻字符串, LVGL 会拷进标签)
         * @param[in] page const char* 点击后跳的页注册名 (常驻; nullptr = 只显示不响应)
         * @param[in] image_src const void* 位图图标源 (lv_image_dsc_t*, 常驻); 与 icon 二选一
         * @return bool 二选一没给全 / 名字为空 / 栅格未建 / 槽位满 返回 false
         * @note
         *  - image_src 非空走位图控件, 否则走字形标签; 两个只能给一个, 运行期定
         *  - 图标高度 = 格子高 × 占比; 名字是图标下面一行, 高度交给字体, 不给百分比框
         *  - 位图用 CONTAIN 缩进那块; 名字单行打点、定宽居中, 撑不出自己那行
         *  - 位置按下标算: col = 下标 % k_app_bar_columns, row = 下标 / k_app_bar_columns
         */
        bool add_app_bar(const char* icon, const char* text, const char* page, const void* image_src = nullptr);

        /**
         * @brief 桌面图标是否显示 (锁屏期间关掉, 只留解锁这一件事)
         * @param[in] vis bool true = 显示
         * @note  隐藏即不可点: indev 命中会直接跳过隐藏对象(lv_indev_search_obj 开头就 return),
         *        所以不必再叠一层 DISABLED; 逐格设是因为隐藏同样不会自动下发到子对象
         */
        void set_app_bar_visible(bool vis);

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 建顶层提示标签 (set_top_text 内部惰性调用)
        bool create_top_label();

        /**
         * @brief 图标点击回调: 按该格的页名排队换页
         * @param[in] e lv_event_t* LVGL 事件 (user_data = this)
         */
        static void app_bar_clicked_cb(lv_event_t* e);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_obj_t* screen    = nullptr; // 透明场景根 (页面父节点)
        lv_obj_t* wallpaper = nullptr; // 壁纸 image (父节点是 display 的 active screen)
        lv_obj_t* top_text  = nullptr; // 顶层提示标签
        lv_obj_t* app_grid  = nullptr; // 图标栅格 (父节点是 screen)

        etl::array<AppBar, k_app_bar_max_count> bars = {}; // 图标槽位: cell == nullptr 表示空槽
        App*      router   = nullptr; // 换页入口 (App::init 注入)
        AppBar*   drag_bar = nullptr; // 拖拽中的格子 (未实现)
        bool      bars_visible = true; // 图标是否显示 (锁屏期间为 false)

        /* 位图图标素材还没有, 先只留这两个 WIP 钩子; 实现后按 grid 单元格挂进去 */
        lv_obj_t* mouse_preview = nullptr; /*鼠标位置只有鼠标时生效*/
        lv_obj_t* hover_obj     = nullptr; /*悬停的对象*/

        void remove_app_bar(etl::string<16>& name);
        void rename_app_bar(etl::string<16>& old_name, etl::string<16>& new_name);
        void set_app_bar_position(etl::string<16>& name, uint8_t position);
    };
}

#endif // DESKTOP_HPP
