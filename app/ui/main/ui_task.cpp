/* SPDX-License-Identifier: Apache-2.0 */
/* UI 线程入口: 组合根(port + 外壳 + 页面) + 线程壳。
 * 目录约定: global/ = 常驻外壳(桌面/顶栏), home/ = 页面(锁屏/设置), indev/ = 输入后端 */
#include "ui_task.hpp"
#include "mini_time.h"
#include "schedule.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>
#include <lvgl/core/lv_timer.h>
#include <lvgl/font/lv_symbol_def.h>

namespace app
{
    constexpr const char* k_ui_thread_name = "UiThread";

    namespace
    {
        /**
         * @brief 起一个一次性定时器 (repeat_count=1, 到期 LVGL 自删)
         * @param[in] cb        lv_timer_cb_t 到期回调
         * @param[in] delay_ms  std::uint32_t 延时 (ms)
         * @param[in] user_data void*         透传给回调的上下文
         */
        void make_demo_timer(lv_timer_cb_t cb, std::uint32_t delay_ms, void* user_data)
        {
            lv_timer_t* timer = lv_timer_create(cb, delay_ms, user_data);
            if (timer != nullptr)
            {
                lv_timer_set_repeat_count(timer, 1);
            }
        }

        /* 演示: 4s 输错 / 8s 输对, 走的是和键盘 ENTER 完全相同的 on_submit 路径 */
        void demo_wrong_password(lv_timer_t* t)
        {
            auto* lock = static_cast<ui::LockScreen*>(lv_timer_get_user_data(t));
            if (lock != nullptr)
            {
                lock->on_submit("123456");
            }
        }

        void demo_right_password(lv_timer_t* t)
        {
            auto* lock = static_cast<ui::LockScreen*>(lv_timer_get_user_data(t));
            if (lock != nullptr)
            {
                lock->on_submit("1234567");
            }
        }
    } // namespace

    UiTask::UiTask(const char* panel_label, std::uint16_t panel_occupy)
        : port_(PanelPort::get_instance(panel_label, panel_occupy))
    {
    }

    bool UiTask::prepare()
    {
        const int ret = this->port_.Init();
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(k_ui_thread_name, "port init failed: %d", ret);
            return false;
        }

        /* 外壳: 桌面/顶栏/输入后端都挂在本屏 display 上, 不碰全局默认屏 */
        if (!this->app_.init(this->port_.disp, *this->input_))
        {
            MT_LOG_ERROR(k_ui_thread_name, "app init failed");
            return false;
        }

        /* 顶栏初始内容: 左段两项(宽度自适应) + 右段一项(固定 44px) */
        ui::TopBar& bar = this->app_.get_top_bar();
        bar.add_item(LV_SYMBOL_WIFI);
        bar.add_item("20:31", 0, true);
        bar.add_item("88%");

        /* 页表注册: 名字由页面自己带, 换页方只写字符串 */
        if (!this->app_.bind_page(this->lock_))
        {
            MT_LOG_ERROR(k_ui_thread_name, "bind page lock failed");
        }
        if (!this->app_.bind_page(this->setting_))
        {
            MT_LOG_ERROR(k_ui_thread_name, "bind page setting failed");
        }

        /* 首页: 锁屏。reset_to 只排队, 真正建控件树发生在下一轮 App::tick */
        this->app_.reset_to("lock");
        MT_LOG_INFO(k_ui_thread_name, "screen ready (%ux%u)", static_cast<unsigned>(this->port_.Width()),
                    static_cast<unsigned>(this->port_.Height()));
        return true;
    }

    void UiTask::thread(void* param)
    {
        auto* self = static_cast<UiTask*>(param);
        if ((self == nullptr) || (self->input_ == nullptr))
        {
            MT_LOG_ERROR(k_ui_thread_name, "ui thread: 实例或输入后端为空, ui thread exit");
            return;
        }

        /* 常驻业务数据(跨页面共用): 只初始化一次, 不归任何一屏 */
        app::LockPassword::get_instance().set("1234567");

        if (!self->prepare())
        {
            MT_LOG_ERROR(k_ui_thread_name, "prepare failed, ui thread exit");
            return;
        }

        make_demo_timer(demo_wrong_password, 4000U, &self->lock_);
        make_demo_timer(demo_right_password, 8000U, &self->lock_);

        for (;;)
        {
            /* 先换页/驱动页面, 再刷屏: 反了会给"这一帧刚建的控件"多留一帧空窗 */
            self->app_.tick();

            /* 全程序唯一驱动点: 这一个 handler 把 display 的刷新和所有 indev 的读取都跑掉。
             * 别在别的线程再调一次 —— LVGL 会因 already_running 直接顶掉, 界面就不刷了 */
            if (self->port_.disp != nullptr)
            {
                lv_timer_handler();
            }

            mini_os_schedule_delay(MINI_OS_MS_TO_TICK(k_loop_period_ms));
        }
    }

    bool UiTask::thread_register(ui::InputBackend& input)
    {
        this->input_ = &input;
        auto handle  = mini_os_thread_create(k_ui_thread_name, k_stack_size, k_priority, thread, this);
        if (!handle)
        {
            this->input_ = nullptr;
            MT_LOG_ERROR(k_ui_thread_name, "Ui thread register failed");
            return false;
        }
        MT_LOG_INFO(k_ui_thread_name, "Ui thread register success");
        return true;
    }

} // namespace app
