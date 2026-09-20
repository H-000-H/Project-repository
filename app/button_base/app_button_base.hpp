/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_button_base.hpp
 * @brief 按键基类 (底层): 模板保类型安全 + 虚函数下发事件, 与具体按键无关
 * @author H-000-H
 * @details   一颗物理按键 = 本模板的一个子类实例 (子类见 app/button):
 *          - 模板参数 CallbackData: 该按键的用户数据, 以**引用**交给回调,
 *            子类里直接 data.xxx 用, 不需要任何 void* 手工强转 (类型安全);
 *          - 纯虚 ButtonCallback(self, data): 子类只需实现这一个函数;
 */
#ifndef APP_BUTTON_BASE_HPP
#define APP_BUTTON_BASE_HPP

#include <cstdint>

#include "compiler_compat.h"
#include "lib/button.hpp"
#include "device.h"
#include "err.h"
#include "log.h"
#include "system_log.h"
#include "vfs-gpio.h"

namespace app
{
constexpr const bool kTriggerLevelLow  = false;
constexpr const bool kTriggerLevelHigh = true;
constexpr const char* kButtonTag = "Button"; 

/**
 * @brief 单颗按键基类 (一个类模板实例 = 一颗物理按键)
 * @tparam CallbackData 该按键的用户数据类型, 由子类在继承时指定
 */
template <typename CallbackData>
class AppButton
{
public:
    /**
     * @brief 构造: 按 label 查找并打开按键 GPIO 设备, 绑定用户数据引用
     * @param label         DTS 节点 label (如 "button1")
     * @param pressed_level 按下时的有效电平 (与 board.dts 的上下拉配套)
     * @param data          该按键的用户数据 (引用, 保证非空, 生命周期须长于本对象)
     */
    AppButton(const char* label, uint8_t pressed_level, CallbackData& data)
        : m_label(label), m_pressed_level(pressed_level), m_data(data)
    {
        ::device* pdev = device_find_by_label(m_label);
        if (IS_ERR_OR_NULL(pdev))
        {
            MT_LOG_WARN(kButtonTag, "%s not found", m_label);
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(kButtonTag, "%s device_open failed", m_label);
            return;
        }
        m_dev = pdev;
    }

    virtual ~AppButton() = default; /**< 虚析构: 支持经基类指针派生销毁 */

    AppButton(const AppButton&) = delete;            /**< 禁用拷贝构造 */
    AppButton& operator=(const AppButton&) = delete; /**< 禁用拷贝赋值 */
    AppButton(AppButton&&) = delete;                 /**< 禁用移动构造 */
    AppButton& operator=(AppButton&&) = delete;      /**< 禁用移动赋值 */

    /**
     * @brief 采样一次: 读本键 GPIO 电平并写入状态机; scan() 由统一驱动调用
     */
    void SampleLevel()
    {
        if (m_dev == nullptr)
            return;

        struct vfs_gpio_arg arg = {0};
        if (device_ioctl(m_dev, GPIO_CMD_GET_LEVEL, &arg, sizeof(arg), 100) != MINI_OK)
        {
            MT_LOG_ERROR(kButtonTag, "%s get level failed", m_label);
            return;
        }
        m_button.set_preesed(arg.level == m_pressed_level ? button::kPressed : button::kNot_Pressed);
    }

    /**
     * @brief 注册库回调 (void* param = this)
     */
    void RegisterCallback()
    {
        m_button.callback_register(CallbackEntry, this);
    }

    /**
     * @brief 事件处理 —— 子类只需实现这一个
     * @param self 触发事件的按钮库对象 (可取 event_read()/read_id())
     * @param data 本按键的用户数据 (类型由模板参数决定)
     * @return 处理结果 (约定返回 true 表示已消费)
     */
    virtual bool ButtonCallback(button::Button& self, CallbackData& data) = 0;

protected:
    button::Button m_button; /**< 每实例一个库对象: 构造即分配 id 并挂入全局链表 */
    ::device*      m_dev = nullptr;
    const char*    m_label;
    uint8_t        m_pressed_level;
    CallbackData&  m_data;

private:
    /**
     * @brief 库回调入口: 转发到虚函数
     * @param param RegisterCallback 传入的 this 指针 (非空)
     * @param self  触发事件的按钮库对象
     * @return 子类 ButtonCallback 的返回值
     */
    static bool CallbackEntry(void* param, button::Button& self)
    {
        /* param 由 RegisterCallback 传入的 this, 不会为空 */
        auto app = static_cast<AppButton*>(param);
        return app->ButtonCallback(self, app->m_data);
    }
};

} // namespace app

#endif // APP_BUTTON_BASE_HPP
