/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_button.cpp
 * @brief 板载按键: 两颗按键子类(只写回调) + 唯一的扫描驱动任务
 * @author H-000-H
 */
#include "app_button.hpp"

#include "app_button_base.hpp"
#include "lib/button.hpp"
#include "schedule.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>
/* LVGL 按键桥接在 app/ui/indev, 该目录不在 include 路径里(只有 app/ui/main), 用相对路径 */
#include "../ui/indev/lvgl_button_bridge.hpp"
namespace app
{
/*按键1*/
/**
 * @brief 按键1 用户数据: 记录按压时长、点击计数与按下时刻
 */
struct ButtonData1
{
    uint32_t trigger_time; /**< 本次按压时长 (ms) */
    uint32_t trigger_num;  /**< 累计点击次数 */
    uint32_t press_tick;   /**< 按下时刻 tick (0 表示未记录) */
};

class AppButton1 : public AppButton<ButtonData1>
{
private:
    ~AppButton1() =default;                  /**< 私有析构: 仅经单例存活 */
    AppButton1(const AppButton1&) = delete;  /**< 禁用拷贝构造 */
    AppButton1(AppButton1&&)=delete;         /**< 禁用移动构造 */
    AppButton1 &operator=(const AppButton1&) = delete; /**< 禁用拷贝赋值 */
    AppButton1 &operator=(AppButton1&&) =delete;       /**< 禁用移动赋值 */
    /**
     * @brief 构造: 绑定 button1 设备与用户数据 (低电平触发)
     * @param data 按键1 用户数据引用
     */
    explicit AppButton1(ButtonData1& data) : AppButton<ButtonData1>("button1", kTriggerLevelLow, data)
    {

    }

    /**
     * @brief 按键1 事件处理: 记录按压时长、累计点击并打印日志
     * @param self 触发事件的按钮库对象
     * @param data 按键1 用户数据
     * @return 恒返回 true (已消费事件)
     */
    bool ButtonCallback(button::Button& self, ButtonData1& data) override final
    {
        const uint32_t now = static_cast<uint32_t>(button::get_tick());

        switch(self.event_read())
        {
        case button::Button_Event::kPressed_Down:
            /* 记录按下时刻 */
            data.press_tick = now;
            MT_LOG_INFO(kName, "pressed");
            break;

        case button::Button_Event::kPressed_Short_Over:
        case button::Button_Event::kPressed_Long_Over:
        case button::Button_Event::kPressed_Long_Hold_Over:
            /* 松开沿: 得到本次按压时长; 没记录过 down(异常沿)就不算, 免得算出个巨大的值 */
            if (data.press_tick != 0U)
            {
                data.trigger_time = now - data.press_tick;
                MT_LOG_INFO(kName, "released, hold :%u ms", static_cast<unsigned>(data.trigger_time));
            }
            break;

        case button::Button_Event::kSignal_Pressed_Click:
            data.trigger_num++;
            MT_LOG_INFO(kName, "click num :%u", static_cast<unsigned>(data.trigger_num));
            break;

        default:
            break; /* Short_Start / Long_Start / Long_Hold 等过程事件不打印 */
        }
        return true;
    }

    static constexpr const char* kName = "Button1";
public:
    /**
     * @brief  获取按键1 单例 (首次调用时构造并绑定静态用户数据)
     * @return 按键1 单例引用
     */
    static AppButton1& GetInstance()
    {
        static ButtonData1 s_data{};
        static AppButton1  s_instance(s_data);
        return s_instance;
    }
};

/**
 * @brief 扫描任务体: 初始化 LVGL 桥接后死循环采样并驱动按键状态机
 * @param param 线程参数 (未使用)
 */
void Button::Thread(void* param)
{
    ui::ButtonLvglInit();

    auto& button1 = AppButton1::GetInstance();
    button1.RegisterCallback();
    for(;;)
    {
        ui::ButtonLvglSample();
        button1.SampleLevel();
        button::Button::scan();
        mini_os_schedule_delay(MINI_OS_MS_TO_TICK(button::kScan_Freq_Ms));  // 50ms

    }
}

/**
 * @brief  注册按键扫描任务 (mini-os 线程)
 * @return 创建成功返回 true, 失败返回 false
 */
bool Button::ThreadRegister()
{
    auto handle = mini_os_thread_create(  kThreadName,
                            kStackSize,
                            kPriority,
                            Thread,
                            nullptr);
    if(!handle)
    {
        MT_LOG_ERROR(kThreadName, "button thread create failed");
        return false;
    }
    MT_LOG_INFO(kThreadName, "button thread create finish");
    return true;
}
} // namespace app
