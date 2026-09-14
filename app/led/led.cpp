/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file led.cpp
 * @brief 板载 LED: 周期翻转任务 + 手动控制接口
 * @author H-000-H
 * @note  任务创建与循环体按后端显式分支 (裸机 xtask / mini-os), 差异不藏进宏。
 */
#include "led.hpp"

#include "device.h"
#include "driver.h"
#include "log.h"
#include "status.h"
#include "system_log.h"
#include "vfs-gpio.h"

#if defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
#include "mini_backend.h"
#endif
#if defined(CONFIG_OS_MINI_OS)
#include "thread.h"
#elif defined(CONFIG_OS_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
#endif

namespace app_led
{

#if defined(CONFIG_OS_BARE) && !defined(CONFIG_XTASK_PREEMPT)
x_task Led::tcb_;
#endif

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

#if defined(CONFIG_OS_BARE)
void Led::Thread(x_task* self)
{
    PT_BEGIN(self);
    while (true)
    {
        BlinkStep();
        PT_DELAY(self, kBlinkPeriodMs);
    }
    PT_END(self);
}
#elif defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
void Led::Thread(void* param)
{
    (void)param;

    while (true)
    {
        BlinkStep();
#if defined(CONFIG_OS_MINI_OS)
        mini_os_thread_delay_ms(kBlinkPeriodMs);
#else
        vTaskDelay(pdMS_TO_TICKS(kBlinkPeriodMs));
#endif
    }
}
#else
#error "led.cpp 尚未适配该 OS 后端: 请补上 Thread 循环与该内核的毫秒延时"
#endif

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
#if defined(CONFIG_OS_BARE) && defined(CONFIG_XTASK_PREEMPT)
    /* 抢占式: TCB 由任务池分配, 需要优先级 */
    x_task_handle_t handle = x_scheduler_task_create(
        kTaskName, kBlinkPeriodMs, kTaskPriority,
        [](x_task* self) { GetInstance().Thread(self); }, nullptr);
    return handle != 0;
#elif defined(CONFIG_OS_BARE)
    /* 协调式: TCB 由本类静态提供, 无优先级概念 */
    x_task_handle_t handle = xscheduler_task_create(
        &tcb_, kTaskName, [](x_task* self) { GetInstance().Thread(self); }, kBlinkPeriodMs);
    return handle != 0;
#elif defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
    /* 真线程后端: 走 mini_backend 统一任务入口 (栈大小只在这里用得上) */
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
#else
#error "led.cpp 尚未适配该 OS 后端: 请补上任务创建分支"
#endif
}

} // namespace app_led
