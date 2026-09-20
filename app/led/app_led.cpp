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

/**
 * @brief  获取 LED 控制器单例
 * @return LED 控制器单例引用
 */
Led& Led::GetInstance()
{
    static Led instance;
    return instance;
}

/**
 * @brief 构造: 按标签查找并打开 led 设备, 失败则记录日志并保持未绑定
 */
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

/**
 * @brief 翻转一次 LED: 周期任务的单步动作, 手动接管或未绑定设备时跳过
 */
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

/**
 * @brief  周期翻转任务体: 死循环, 每 kBlinkPeriodMs 翻转一次
 * @param  param 线程参数 (未使用)
 */
void Led::Thread(void* param)
{
    (void)param;

    while (true)
    {
        BlinkStep();
        mini_os_thread_delay_ms(kBlinkPeriodMs);
    }
}

/**
 * @brief  设置 LED 输出电平
 * @param  lit true 点亮, false 熄灭
 * @return 设置成功返回 true, 设备未绑定或 ioctl 失败返回 false
 */
bool Led::ApplyLit(bool lit)
{
    if (m_dev == nullptr)
        return false;

    struct vfs_gpio_arg arg = {0};
    arg.level = lit;
    return device_ioctl(m_dev, GPIO_CMD_SET_LEVEL, &arg, sizeof(arg), 100) == MINI_OK;
}

/**
 * @brief  点亮 LED 并停止周期翻转 (手动接管)
 * @return 成功返回 true, 设备未绑定或设置失败返回 false
 */
bool Led::TurnOn()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = true; /* 先接管, 再设电平 */
    return ApplyLit(true);
}

/**
 * @brief  熄灭 LED 并停止周期翻转 (手动接管)
 * @return 成功返回 true, 设备未绑定或设置失败返回 false
 */
bool Led::TurnOff()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = true;
    return ApplyLit(false);
}

/**
 * @brief  交还控制权: 下一轮周期任务恢复翻转
 * @return 设备已绑定返回 true, 未绑定返回 false
 */
bool Led::ResumeBlink()
{
    if (m_dev == nullptr)
        return false;

    m_manual_hold = false; /* 交还控制权: 下一轮 Thread 恢复翻转 */
    return true;
}

/**
 * @brief  注册 LED 周期翻转任务到调度器 (mini-os 线程)
 * @return 创建成功返回 true, 失败返回 false
 */
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
