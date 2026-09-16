/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file button_indev.hpp
 * @brief 把板载按键作为界面输入设备: 按键子类 + 单例
 * @author H-000-H
 * @details AppButton 的模板参数是"这颗键的用户数据类型"(不是按键类自己), 回调按引用拿到它,
 *          不用 void* 强转。界面侧只关心"沿"(按下/抬起): 按键库的 kSignal_Pressed_Click
 *          要等满 500ms 多击窗口才结算, 拿它驱动界面会慢半拍。
 */
#ifndef BUTTON_INDEV_HPP
#define BUTTON_INDEV_HPP
#include "mini_backend.h"
#include "thread.h"
#include <cstdint>
#include "app_button_base.hpp"
#include "lib/button.hpp" /* button::Button_Event / button::get_tick */
#include "lvgl.h"
namespace app
{
struct IndevButtonData
{
    
};

class IndevButton1 : public AppButton<IndevButtonData>
{
public:
    /** @brief 单例 (app/ 的命名约定是 PascalCase 方法, 同 app::Ui::GetInstance) */
    static IndevButton1& GetInstance()
    {
        static IndevButtonData s_data{};
        static IndevButton1    s_instance(s_data);
        return s_instance;
    }

private:
    /**
     * @param data 本键的用户数据 (引用; 由 GetInstance() 里的静态对象提供)
     */
    explicit IndevButton1(IndevButtonData& data)
        : AppButton<IndevButtonData>("button1", kTriggerLevelLow, data)
    {
    }

    ~IndevButton1() = default;
    IndevButton1(const IndevButton1&) = delete;
    IndevButton1& operator=(const IndevButton1&) = delete;
    IndevButton1(IndevButton1&&) = delete;
    IndevButton1& operator=(IndevButton1&&) = delete;

    bool ButtonCallback(button::Button& self, IndevButtonData& data) override final
    {
        switch (self.event_read())
        {
        case button::Button_Event::kPressed_Down:
            data.pressed   = true;
            data.last_tick = static_cast<uint32_t>(button::get_tick());
            break;

        case button::Button_Event::kPressed_Short_Over:
        case button::Button_Event::kPressed_Long_Over:
        case button::Button_Event::kPressed_Long_Hold_Over:
            data.pressed   = false;
            data.last_tick = static_cast<uint32_t>(button::get_tick());
            break;

        default:
            break; /* 过程事件(Short_Start/Long_Start/Long_Hold)与多击结算事件不参与界面输入 */
        }
        return true;
    }
};
} // namespace app
#endif // BUTTON_INDEV_HPP