/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_button.cpp
 * @brief 板载按键: 两颗按键子类(只写回调) + 唯一的扫描驱动任务
 * @author H-000-H
 */
#include "app_button.hpp"

#include "app_button_base.hpp"
#include "driver.h"
#include "lib/button.hpp"
#include "schedule.h"
#include "status.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>
#include <sys/_intsup.h>

namespace app
{
/*按键1*/
struct ButtonData1
{
    uint32_t trigger_time;
    uint32_t trigger_num;
    uint32_t press_tick;
};

class AppButton1 : public AppButton<ButtonData1>
{
private:
    ~AppButton1() =default;
    AppButton1(const AppButton1&) = delete;
    AppButton1(AppButton1&&)=delete;
    AppButton1 &operator=(const AppButton1&) = delete;
    AppButton1 &operator=(AppButton1&&) =delete;
    explicit AppButton1(ButtonData1& data) : AppButton<ButtonData1>("button1", Button::kTriggerLevelLow, data)
    {

    }

    bool ButtonCallback(button::Button& self, ButtonData1& data) override final
    {
        const uint32_t now = static_cast<uint32_t>(button::get_tick());

        switch(self.event_read())
        {
        case button::Button_Event::kPressed_Down:
            /* 记录按下时刻 */
            data.press_tick = now;
            MT_LOG_INFO(name, "pressed");
            break;

        case button::Button_Event::kPressed_Short_Over:
        case button::Button_Event::kPressed_Long_Over:
        case button::Button_Event::kPressed_Long_Hold_Over:
            /* 松开沿: 得到本次真实按压时长 */
            data.trigger_time = now - data.press_tick;
            MT_LOG_INFO(name, "released, hold :%u ms", data.trigger_time);
            break;

        case button::Button_Event::kSignal_Pressed_Click:
            data.trigger_num++;
            MT_LOG_INFO(name, "click num :%u", data.trigger_num);
            break;

        default:
            break; /* Short_Start / Long_Start / Long_Hold 等过程事件不打印 */
        }
        return true;
    }

    const char* name = "Button1";
public:
    static AppButton1& GetInstance()
    {
        static ButtonData1 s_data{};
        static AppButton1  s_instance(s_data);
        return s_instance;
    }
};

struct ButtonData2
{
    uint32_t press_tick;   /* 本次按下时刻  */
    uint32_t hold_time;    /* 最近一次按压时长 (ms) */
    uint32_t click_num;    /* 单击累计 */
    uint32_t double_num;   /* 双击累计 */
    uint32_t repeat_num;   /* 连击累计 */
};

class AppButton2 : public AppButton<ButtonData2>
{
private:
    ~AppButton2() =default;
    AppButton2(const AppButton2&) = delete;
    AppButton2(AppButton2&&)=delete;
    AppButton2 &operator=(const AppButton2&) = delete;
    AppButton2 &operator=(AppButton2&&) =delete;
    explicit AppButton2(ButtonData2& data) : AppButton<ButtonData2>("button2", Button::kTriggerLevelLow, data)
    {

    }

    bool ButtonCallback(button::Button& self, ButtonData2& data) override final
    {
        const uint32_t now = static_cast<uint32_t>(button::get_tick());

        switch(self.event_read())
        {
        case button::Button_Event::kPressed_Down:
            data.press_tick = now;
            MT_LOG_INFO(name, "pressed");
            break;

        case button::Button_Event::kPressed_Long_Start:
            /* 按住累计 >= 1000ms: 长按 */
            MT_LOG_INFO(name, "long press");
            break;

        case button::Button_Event::kPressed_Long_Hold:
            /* 按住累计 >= 3000ms: 超长按 */
            MT_LOG_INFO(name, "long hold");
            break;

        case button::Button_Event::kPressed_Short_Over:
        case button::Button_Event::kPressed_Long_Over:
        case button::Button_Event::kPressed_Long_Hold_Over:
            /* 松开沿: 得到本次真实按压时长 */
            data.hold_time = now - data.press_tick;
            MT_LOG_INFO(name, "released, hold :%u ms", data.hold_time);
            break;

        case button::Button_Event::kSignal_Pressed_Click:
            data.click_num++;
            MT_LOG_INFO(name, "single click num :%u", data.click_num);
            break;

        case button::Button_Event::kDouble_Pressed_Click:
            data.double_num++;
            MT_LOG_INFO(name, "double click num :%u", data.double_num);
            break;

        case button::Button_Event::kPressed_Repeat_Click:
            data.repeat_num++;
            MT_LOG_INFO(name, "repeat click num :%u", data.repeat_num);
            break;

        default:
            break; /* kPressed_Short_Start 过程事件不打印 */
        }
        return true;
    }

    const char* name = "Button2";
public:
    static AppButton2& GetInstance()
    {
        static ButtonData2 s_data={};
        static AppButton2  instance(s_data);
        return instance;
    }
};

void Button::Thread(void* param)
{
    auto& button1 = AppButton1::GetInstance();
    auto& button2 = AppButton2::GetInstance();
    button1.RegisterCallback();
    button2.RegisterCallback();
    for(;;)
    {
        button2.SampleLevel();
        button1.SampleLevel();
        button::Button::scan();
        mini_os_schedule_delay(MINI_OS_MS_TO_TICK(button::kScan_Freq_Ms));  // 50ms

    }
}

bool Button::ThreadRegister()
{
    mini_os_thread_create(  kThreadName, 
                            kStackSize, 
                            kPriority, 
                            Thread, 
                            nullptr);
    MT_LOG_INFO(kThreadName, "button thread create finish");
    return true;
}
} // namespace app
