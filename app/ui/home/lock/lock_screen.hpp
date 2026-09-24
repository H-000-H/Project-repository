/**
 * @file lock_screen.hpp
 * @author H-000-H
 * @brief 锁屏页面: 控件树 + 页面状态 + 事件处理都在这儿
 * @note   持久数据(密码/锁定标志)不放这儿, 走 app::LockPassword 这个跨页面常驻服务 —— 页面删了它还在
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOCK_SCREEN_HPP
#define LOCK_SCREEN_HPP
#include "lock-password.hpp"
#include "lvgl/lvgl.h"
#include "page.hpp"
#include <cstdint>

namespace ui
{
    class App;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 锁屏页面状态机
    enum class LockState : std::uint8_t
    {
        LOCKED,    // 等待输入
        UNLOCKING, // 密码正确, Welcome 提示显示中
        UNLOCKED   // 提示已收走, 页面可撤
    };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 锁屏页; 默认收场方式 DESTROY (每次进来重头输密码), 注册名 "lock"
    class LockScreen : public Page
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 构造
         * @param[in] app App& 组合根引用, 页面靠它访问外壳与焦点组
         */
        explicit LockScreen(App& app);
        ~LockScreen() override;

        LockScreen(const LockScreen&)            = delete;
        LockScreen& operator=(const LockScreen&) = delete;
        LockScreen(LockScreen&&)                 = delete;
        LockScreen& operator=(LockScreen&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        void enter(lv_obj_t* parent) override;
        void exit() override;
        bool is_finished() const override { return this->state == LockState::UNLOCKED; }

        /**
         * @brief 提交一次密码 (输入框 ENTER 事件与外部模拟输入都走这里)
         * @param[in] input const char* 输入内容, 为空指针直接忽略
         */
        void on_submit(const char* input);

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 建锁屏控件树
         * @param[in] parent lv_obj_t* 挂载父对象
         * @return bool 建成功返回 true
         */
        bool create_widgets(lv_obj_t* parent);

        // 拆控件树; lv_obj_delete 会连带把子控件从焦点组摘掉
        void destroy_widgets();

        // 把输入框加进焦点组并聚焦
        void grab_focus();

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 显示提示文字并起 1.5s 一次性定时器
         * @param[in] text  const char* 提示内容
         * @param[in] color lv_color_t  文字颜色
         */
        void show_tip(const char* text, lv_color_t color);

        // 收走提示文字与定时器
        void clear_tip();

        // 提示定时器到期: 收提示, 成功路径推进到 UNLOCKED
        void on_tip_timeout();

        /**
         * @brief LVGL 事件回调适配: 原样转发给 on_submit
         * @param[in] e lv_event_t* LVGL 事件 (user_data = this)
         */
        static void enter_event_cb(lv_event_t* e);

        /**
         * @brief LVGL 定时器回调适配: 原样转发给 on_tip_timeout
         * @param[in] t lv_timer_t* LVGL 定时器 (user_data = this)
         */
        static void tip_timer_cb(lv_timer_t* t);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        App&               app;                                             // 组合根: 外壳与焦点组的入口
        app::LockPassword& store      = app::LockPassword::get_instance();  // 常驻密码服务
        LockState          state      = LockState::LOCKED;                  // 页面状态
        std::uint8_t       fail_count = 0U;                                 // 连续失败次数

        /* 控件树根由基类 Page 持有, 这里只留派生类要单独动的两个 */
        lv_obj_t*   textarea  = nullptr; // 密码输入框 (根的第一个子节点)
        lv_obj_t*   tip_label = nullptr; // 提示标签
        lv_timer_t* tip_timer = nullptr; // 提示 1.5s 一次性定时器
        lv_style_t  style_glass;         // 玻璃卡片样式
    };
}

#endif // LOCK_SCREEN_HPP
