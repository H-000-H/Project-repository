/**
 *@file led.cpp
 *@brief LED 周期任务实现
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@author H-000-H
 */
#include "led.hpp"
#include "device.h"
#include "driver.h"
#include "log.h"
#include "status.h"
#include "system_log.h"
#include "vfs-gpio.h"

namespace App_Led
{
#ifndef CONFIG_XTASK_PREEMPT
    x_task Led::s_tcb;
#endif

    Led& Led::get_instance()
    {
        static Led s_instance;
        return s_instance;
    }

    Led::Led()
    {
        ::device* pdev = device_find_by_label("led");
        if (IS_ERR_OR_NULL(pdev))
        {
            MT_LOG_WARN(kTag, "led device not found");
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(kTag, "device_open failed");
            return;
        }
        m_dev = pdev;
    }

    void Led::thread(x_task* self)
    {
        struct vfs_gpio_arg arg = {0};

        PT_BEGIN(self);
        while (true)
        {
            if (m_dev != nullptr)
            {
                int ret = device_ioctl(m_dev, GPIO_CMD_TOGGLE, &arg, sizeof(arg), 100);
                MT_LOG_INFO(kTag,"%d",mini_log_get_tick());
                if (ret != MINI_OK)
                    MT_LOG_ERROR(kTag, "ioctl failed: %d", ret);
            }
            PT_DELAY(self, kDelay);
        }
        PT_END(self);
    }

    bool Led::thread_register(void)
    {
#ifdef CONFIG_XTASK_PREEMPT
        x_task_handle_t handle = x_scheduler_task_create(
            kName, kDelay, kPriority,
            [](x_task* self) { get_instance().thread(self); }, nullptr);
#else
        x_task_handle_t handle = xscheduler_task_create(
            &s_tcb, kName,
            [](x_task* self) { get_instance().thread(self); },
            kDelay);
#endif
        return handle != 0;
    }

} // namespace App_Led
