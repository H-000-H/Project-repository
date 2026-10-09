/**
 * @file app_led.cpp
 * @author H-000-H
 * @brief 板载 LED: 周期翻转任务 + 手动控制接口
 * @note  任务后端固定为 mini-os; 接口说明见 app_led.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "app_led.hpp"
#include "device.h"
#include "driver.h"
#include "log.h"
#include "status.h"
#include "system_log.h"
#include "thread.h"
#include "vfs-gpio.h"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    Led& Led::get_instance()
    {
        static Led instance;
        return instance;
    }

    Led::Led()
    {
        ::device* pdev = device_find_by_label("led");
        if (IS_ERR_OR_NULL(pdev))
        {
            MT_LOG_WARN(k_tag, "led device not found");
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "device_open failed");
            return;
        }
        this->m_dev = pdev;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void Led::blink_step()
    {
        // 手动接管期间不翻转, 避免覆盖 turn_on/turn_off 的结果
        if ((this->m_dev == nullptr) || this->m_manual_hold)
        {
            return;
        }

        struct vfs_gpio_arg arg = {0};
        const int ret           = device_ioctl(this->m_dev, GPIO_CMD_TOGGLE, &arg, sizeof(arg), 100);
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "toggle failed: %d", ret);
        }
    }

    // 周期翻转任务体: 死循环, 每 k_blink_period_ms 翻转一次
    void Led::thread(void* param)
    {
        (void)param;

        for (;;)
        {
            this->blink_step();
            mini_os_thread_delay_ms(k_blink_period_ms);
        }
    }

    // 设置输出电平: 设备未绑定或 ioctl 失败返回 false
    bool Led::apply_lit(bool lit)
    {
        if (this->m_dev == nullptr)
        {
            return false;
        }

        struct vfs_gpio_arg arg = {0};
        arg.level               = lit;
        return device_ioctl(this->m_dev, GPIO_CMD_SET_LEVEL, &arg, sizeof(arg), 100) == MINI_OK;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool Led::turn_on()
    {
        if (this->m_dev == nullptr)
        {
            return false;
        }

        this->m_manual_hold = true; // 先接管, 再设电平
        return this->apply_lit(true);
    }

    bool Led::turn_off()
    {
        if (this->m_dev == nullptr)
        {
            return false;
        }

        this->m_manual_hold = true;
        return this->apply_lit(false);
    }

    bool Led::resume_blink()
    {
        if (this->m_dev == nullptr)
        {
            return false;
        }

        this->m_manual_hold = false; // 交还控制权: 下一轮 thread 恢复翻转
        return true;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool Led::thread_register()
    {
        mini_os_thread_t* handle = mini_os_thread_create(
            k_task_name, k_task_stack, static_cast<mini_os_uint8_t>(k_task_priority),
            [](void* param) { get_instance().thread(param); }, nullptr);
        if (handle == nullptr)
        {
            MT_LOG_ERROR(k_tag, "task register failed");
            return false;
        }
        MT_LOG_INFO(k_tag, "task registered");
        return true;
    }

} // namespace app
