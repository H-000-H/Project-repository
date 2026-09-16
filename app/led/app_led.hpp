/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_led.hpp
 * @brief 板载 LED: 周期翻转任务 + 手动控制接口
 * @author H-000-H
 */
#ifndef APP_LED_APP_LED_HPP_
#define APP_LED_APP_LED_HPP_

#include <cstdint>

struct device; /* 前向声明 C 结构体 */

namespace app
{

/**
 * @brief 板载 LED 控制器 (单例)
 * @note  两个控制来源, 手动优先:
 *        - 周期任务: 默认每 kBlinkPeriodMs 翻转一次 (心跳)
 *        - 手动接口: TurnOn()/TurnOff() 接管后停止翻转;
 *        - ResumeBlink() 交还控制权, 下一轮恢复翻转
 */
class Led
{
public:
    static Led& GetInstance();

    Led(const Led&) = delete;
    Led& operator=(const Led&) = delete;
    Led(Led&&) = delete;
    Led& operator=(Led&&) = delete;

    /** @brief 点亮并停止周期翻转 (手动接管) */
    bool TurnOn();
    /** @brief 熄灭并停止周期翻转 (手动接管) */
    bool TurnOff();
    /** @brief 交还控制权: 下一轮周期任务恢复翻转 */
    bool ResumeBlink();

    void Thread(void* param);

    /** @brief 注册本任务到调度器 */
    bool ThreadRegister();

private:
    Led();

    /** @brief 设置输出电平 (lit=true 点亮) */
    bool ApplyLit(bool lit);
    /** @brief 翻转一次: 周期任务的单步动作, 两后端分支共用 */
    void BlinkStep();

    ::device* m_dev = nullptr;
    /* 手动接管期间不翻转, 避免覆盖 TurnOn/TurnOff 的结果 */
    bool m_manual_hold = false;

    static constexpr unsigned int kBlinkPeriodMs = 500; /**< 心跳翻转周期 (ms) */

    static constexpr unsigned int  kTaskPriority = 12;   /* mini-os: 数值越小越优先 (与通信同级) */
    static constexpr std::uint32_t kTaskStack    = 2048; /* mini-os 线程栈 (字节) */
    static constexpr const char*  kTaskName     = "Led_Task";
    static constexpr const char*  kTag          = "Led";
};

} // namespace app

#endif // APP_LED_APP_LED_HPP_
