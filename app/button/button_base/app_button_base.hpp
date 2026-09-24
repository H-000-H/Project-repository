/**
 * @file app_button_base.hpp
 * @author H-000-H
 * @brief 按键基类 (底层): 模板保类型安全 + 虚函数下发事件, 与具体按键无关
 * @details 一颗物理按键 = 本模板的一个子类实例 (子类见 app/button)。用户数据以**引用**交给
 *          回调, 子类里直接 data.xxx 用, 不需要 void* 强转; 子类只需实现 button_callback。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_BUTTON_BASE_HPP
#define APP_BUTTON_BASE_HPP
#include <cstdint>

#include "compiler_compat.h"
#include "device.h"
#include "err.h"
#include "lib/button.hpp"
#include "log.h"
#include "system_log.h"
#include "vfs-gpio.h"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    constexpr const bool  k_trigger_level_low  = false;
    constexpr const bool  k_trigger_level_high = true;
    constexpr const char* k_button_tag         = "Button";

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    /**
     * @brief 单颗按键基类 (一个类模板实例 = 一颗物理按键)
     * @tparam CallbackData 该按键的用户数据类型, 由子类在继承时指定
     */
    template <typename CallbackData>
    class AppButton
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 构造: 按 label 查找并打开按键 GPIO 设备, 绑定用户数据引用
         * @param[in] label         const char* DTS 节点 label (如 "button1")
         * @param[in] pressed_level uint8_t     按下时的有效电平 (与 board.dts 的上下拉配套)
         * @param[in] data          CallbackData& 该按键的用户数据 (生命周期须长于本对象)
         */
        AppButton(const char* label, uint8_t pressed_level, CallbackData& data)
            : m_label(label), m_pressed_level(pressed_level), m_data(data)
        {
            ::device* pdev = device_find_by_label(m_label);
            if (IS_ERR_OR_NULL(pdev))
            {
                MT_LOG_WARN(k_button_tag, "%s not found", m_label);
                return;
            }
            if (device_open(pdev, nullptr) != MINI_OK)
            {
                MT_LOG_ERROR(k_button_tag, "%s device_open failed", m_label);
                return;
            }
            m_dev = pdev;
        }

        virtual ~AppButton() = default;

        AppButton(const AppButton&)            = delete;
        AppButton& operator=(const AppButton&) = delete;
        AppButton(AppButton&&)                 = delete;
        AppButton& operator=(AppButton&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 采样一次: 读本键 GPIO 电平并写入状态机; scan() 由统一驱动调用
        void sample_level()
        {
            if (m_dev == nullptr)
                return;

            struct vfs_gpio_arg arg = {0};
            if (device_ioctl(m_dev, GPIO_CMD_GET_LEVEL, &arg, sizeof(arg), 100) != MINI_OK)
            {
                MT_LOG_ERROR(k_button_tag, "%s get level failed", m_label);
                return;
            }
            m_button.set_preesed(arg.level == m_pressed_level ? button::kPressed : button::kNot_Pressed);
        }

        // 注册库回调 (void* param = this)
        void register_callback()
        {
            m_button.callback_register(callback_entry, this);
        }

        /**
         * @brief 事件处理 —— 子类只需实现这一个
         * @param[in] self button::Button& 触发事件的按钮库对象
         * @param[in] data CallbackData&   本按键的用户数据
         * @return bool 约定返回 true 表示已消费
         */
        virtual bool button_callback(button::Button& self, CallbackData& data) = 0;

    protected:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        button::Button m_button; // 每实例一个库对象: 构造即分配 id 并挂入全局链表
        ::device*      m_dev = nullptr;
        const char*    m_label;
        uint8_t        m_pressed_level;
        CallbackData&  m_data;

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 库回调入口: 转发到虚函数
         * @param[in] param void*             register_callback 传入的 this (非空)
         * @param[in] self  button::Button&   触发事件的按钮库对象
         * @return bool 子类 button_callback 的返回值
         */
        static bool callback_entry(void* param, button::Button& self)
        {
            auto app = static_cast<AppButton*>(param);
            return app->button_callback(self, app->m_data);
        }
    };
} // namespace app

#endif // APP_BUTTON_BASE_HPP
