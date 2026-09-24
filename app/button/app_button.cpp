/**
 * @file app_button.cpp
 * @author H-000-H
 * @brief 板载按键: 具体按键子类(只写回调) + 唯一的扫描驱动任务
 * @note  上层桥接(按键事件 → LVGL 键值)由装配方注入: 本模块不认识 LVGL。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "app_button.hpp"

#include "app_button_base.hpp"
#include "lib/button.hpp"
#include "schedule.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 按键1 用户数据: 记录按压时长、点击计数与按下时刻
    struct ButtonData1
    {
        std::uint32_t trigger_time; // 本次按压时长 (ms)
        std::uint32_t trigger_num;  // 累计点击次数
        std::uint32_t press_tick;   // 按下时刻 tick (0 表示未记录)
    };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 板载按键1: 只写回调, 生灭交给单例
    class AppButton1 : public AppButton<ButtonData1>
    {
    private:
        ~AppButton1() = default;                           // 私有析构: 仅经单例存活
        AppButton1(const AppButton1&)            = delete; // 禁用拷贝构造
        AppButton1(AppButton1&&)                 = delete; // 禁用移动构造
        AppButton1& operator=(const AppButton1&) = delete; // 禁用拷贝赋值
        AppButton1& operator=(AppButton1&&)      = delete; // 禁用移动赋值

        static constexpr const char* k_name = "Button1"; // 日志标签

        /**
         * @brief 构造: 绑定 button1 设备与用户数据 (低电平触发)
         * @param[in] data ButtonData1& 按键1 用户数据
         */
        explicit AppButton1(ButtonData1& data) : AppButton<ButtonData1>("button1", k_trigger_level_low, data)
        {
        }

        /**
         * @brief 按键1 事件处理: 记录按压时长、累计点击并打印日志
         * @param[in] self button::Button& 触发事件的按钮库对象
         * @param[in] data ButtonData1&    按键1 用户数据
         * @return bool 恒返回 true (已消费事件)
         */
        bool button_callback(button::Button& self, ButtonData1& data) override final
        {
            const std::uint32_t now = static_cast<std::uint32_t>(button::get_tick());

            switch (self.event_read())
            {
            case button::Button_Event::kPressed_Down:
                data.press_tick = now; // 记录按下时刻
                MT_LOG_INFO(k_name, "pressed");
                break;

            case button::Button_Event::kPressed_Short_Over:
            case button::Button_Event::kPressed_Long_Over:
            case button::Button_Event::kPressed_Long_Hold_Over:
                // 松开沿: 得到本次按压时长; 没记录过 down(异常沿)就不算, 免得算出个巨大的值
                if (data.press_tick != 0U)
                {
                    data.trigger_time = now - data.press_tick;
                    MT_LOG_INFO(k_name, "released, hold :%u ms", static_cast<unsigned>(data.trigger_time));
                }
                break;

            case button::Button_Event::kSignal_Pressed_Click:
                data.trigger_num++;
                MT_LOG_INFO(k_name, "click num :%u", static_cast<unsigned>(data.trigger_num));
                break;

            default:
                break; // Short_Start / Long_Start / Long_Hold 等过程事件不打印
            }
            return true;
        }

    public:
        /**
         * @brief 获取按键1 单例 (首次调用时构造并绑定静态用户数据)
         * @return AppButton1& 唯一实例
         */
        static AppButton1& get_instance()
        {
            static ButtonData1 s_data{};
            static AppButton1  s_instance(s_data);
            return s_instance;
        }
    };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 钩子默认全空: 没注入时本任务只驱动板载自己那颗按键 (button1)
    Button::HookFn Button::s_open   = nullptr;
    Button::HookFn Button::s_sample = nullptr;
    void*          Button::s_ctx    = nullptr;

    void Button::set_sample_hook(HookFn open, HookFn sample, void* ctx)
    {
        s_open   = open;
        s_sample = sample;
        s_ctx    = ctx;
    }

    // 按键扫描任务体: 先采上层那颗按键(如 button2), 再采本模块的 button1, 最后统一推进状态机
    void Button::thread(void* param)
    {
        (void)param;

        if (s_open != nullptr)
        {
            s_open(s_ctx);
        }

        auto& button1 = AppButton1::get_instance();
        button1.register_callback();
        for (;;)
        {
            if (s_sample != nullptr)
            {
                s_sample(s_ctx);
            }
            button1.sample_level();
            button::Button::scan();
            mini_os_schedule_delay(MINI_OS_MS_TO_TICK(button::kScan_Freq_Ms)); // 50ms
        }
    }

    bool Button::thread_register()
    {
        auto handle = mini_os_thread_create(k_thread_name,
                                            k_stack_size,
                                            k_priority,
                                            thread,
                                            nullptr);
        if (!handle)
        {
            MT_LOG_ERROR(k_thread_name, "button thread create failed");
            return false;
        }
        MT_LOG_INFO(k_thread_name, "button thread create finish");
        return true;
    }
} // namespace app
