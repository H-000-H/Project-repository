/**
 * @file ui_task.cpp
 * @author H-000-H
 * @brief UI 线程入口: 组合根(port + 外壳 + 页面) + 线程壳
 * @note  不允许在这里写逻辑: 换页与业务都写在各自页面里, 这里只做组合与驱动
 *        目录约定: global/ = 常驻外壳(桌面/顶栏), home/ = 页面(锁屏/设置), indev/ = 输入后端
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
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
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 线程名 (日志用)
    constexpr const char* k_ui_thread_name = "UiThread";
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    UiTask::UiTask(const char* panel_label, std::uint16_t panel_occupy) 
        : port_(PanelPort::get_instance(panel_label, panel_occupy))
    {
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
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

        /* 桌面图标: 名字与页名都是常驻字符串, 点击由 Desktop 排队换页 */
        this->app_.get_desktop().add_app_bar(LV_SYMBOL_SETTINGS, "Setting", "setting");

        /* 首页: 锁屏。reset_to 只排队, 真正建控件树发生在下一轮 App::tick */
        this->app_.reset_to("lock");
        MT_LOG_INFO(k_ui_thread_name, "screen ready (%ux%u)", static_cast<unsigned>(this->port_.Width()),
                    static_cast<unsigned>(this->port_.Height()));
        return true;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void UiTask::thread(void* param)
    {
        auto* self = static_cast<UiTask*>(param);
        if ((self == nullptr) || (self->input_ == nullptr))
        {
            MT_LOG_ERROR(k_ui_thread_name, "ui thread: 实例或输入后端为空, ui thread exit");
            return;
        }

        /* 锁屏密码*/
        app::LockPassword::get_instance().set("1234567");

        if (!self->prepare())
        {
            MT_LOG_ERROR(k_ui_thread_name, "prepare failed, ui thread exit");
            return;
        }

        for (;;)
        {
            /* 先换页/驱动页面, 再刷屏 */
            self->app_.tick();

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
