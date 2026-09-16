/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_led.cpp
 * @brief 板载 LED: 周期翻转任务 + 手动控制接口
 * @author H-000-H
 * @note  任务后端固定为 mini-os。
 */
#include "app_led.hpp"
#include "device.h"
#include "driver.h"
#include "log.h"
#include "status.h"
#include "system_log.h"
#include "vfs-gpio.h"

#include "thread.h"

namespace app
{

Led& Led::GetInstance()
{
    static Led instance;
    return instance;
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

void Led::BlinkStep()
{
    if ((m_dev == nullptr) || (m_manual_hold))
        return;

    struct vfs_gpio_arg arg = {0};
    const int ret = device_ioctl(m_dev, GPIO_CMD_TOGGLE, &arg, sizeof(arg), 100);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "toggle failed: %d", ret);
    }
}

void Led::Thread(void* param)
{
    (void)param;

    while (true)
    {
        BlinkStep();
        mini_os_thread_delay_ms(kBlinkPeriodMs);
    }
}

bool Led::ApplyLit(bool lit)
{
    if (m_dev == nullptr)
        return false;

    struct vfs_gpio_arg arg = {0};
    arg.level = lit;
    return device_ioctl(m_dev, GPIO_CMD_SET_LEVEL, &arg, sizeof(arg), 100) == MINI_OK;
}

bool Led::TurnOn()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = true; /* 先接管, 再设电平 */
    return ApplyLit(true);
}

bool Led::TurnOff()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = true;
    return ApplyLit(false);
}

bool Led::ResumeBlink()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = false; /* 交还控制权: 下一轮 Thread 恢复翻转 */
    return true;
}

bool Led::ThreadRegister()
{
    mini_os_thread_t* handle = mini_os_thread_create(
        kTaskName, kTaskStack, static_cast<mini_os_uint8_t>(kTaskPriority),
        [](void* param) { GetInstance().Thread(param); }, nullptr);
    if (handle == nullptr)
    {
        MT_LOG_ERROR(kTag, "task register failed");
        return false;
    }
    MT_LOG_INFO(kTag, "task registered");
    return true;
}

} // namespace app
