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

#include "mini_backend.h"
#include "thread.h"

namespace app_led
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
    dev_ = pdev;
}

void Led::BlinkStep()
{
    if ((dev_ == nullptr) || (manual_hold_))
        return;

    struct vfs_gpio_arg arg = {0};
    const int ret = device_ioctl(dev_, GPIO_CMD_TOGGLE, &arg, sizeof(arg), 100);
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
    if (dev_ == nullptr)
        return false;

    struct vfs_gpio_arg arg = {0};
    arg.level = lit;
    return device_ioctl(dev_, GPIO_CMD_SET_LEVEL, &arg, sizeof(arg), 100) == MINI_OK;
}

bool Led::TurnOn()
{
    if (dev_ == nullptr)
        return false;

    manual_hold_ = true; /* 先接管, 再设电平 */
    return ApplyLit(true);
}

bool Led::TurnOff()
{
    if (dev_ == nullptr)
        return false;

    manual_hold_ = true;
    return ApplyLit(false);
}

bool Led::ResumeBlink()
{
    if (dev_ == nullptr)
        return false;

    manual_hold_ = false; /* 交还控制权: 下一轮 Thread 恢复翻转 */
    return true;
}

bool Led::ThreadRegister()
{
    /* 走 mini_backend 统一任务入口 (栈大小只在这里用得上) */
    mini_task_handle_t handle = nullptr;
    const mt_err_t ret = mini_task_create_handle(
        kTaskName, app_config::kLedTaskStack, kTaskPriority,
        [](void* param) { GetInstance().Thread(param); }, nullptr, -1, &handle);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "task register failed: %d", static_cast<int>(ret));
        return false;
    }
    MT_LOG_INFO(kTag, "task registered");
    return true;
}

} // namespace app_led
