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
    /**
     * @brief  获取 LED 控制器单例
     * @return LED 控制器单例引用
     */
    static Led& GetInstance();

    Led(const Led&) = delete;            /**< 禁用拷贝构造 */
    Led& operator=(const Led&) = delete; /**< 禁用拷贝赋值 */
    Led(Led&&) = delete;                 /**< 禁用移动构造 */
    Led& operator=(Led&&) = delete;      /**< 禁用移动赋值 */

    /**
     * @brief  点亮并停止周期翻转 (手动接管)
     * @return 成功返回 true, 设备未绑定或设置失败返回 false
     */
    bool TurnOn();
    /**
     * @brief  熄灭并停止周期翻转 (手动接管)
     * @return 成功返回 true, 设备未绑定或设置失败返回 false
     */
    bool TurnOff();
    /**
     * @brief  交还控制权: 下一轮周期任务恢复翻转
     * @return 设备已绑定返回 true, 未绑定返回 false
     */
    bool ResumeBlink();

    /**
     * @brief 周期翻转任务体: 死循环, 每 kBlinkPeriodMs 翻转一次
     * @param param 线程参数 (未使用)
     */
    void Thread(void* param);

    /**
     * @brief  注册本任务到调度器
     * @return 创建成功返回 true, 失败返回 false
     */
    bool ThreadRegister();

private:
    /**
     * @brief 构造: 按标签查找并打开 led 设备, 失败则记录日志并保持未绑定
     */
    Led();

    /**
     * @brief  设置输出电平
     * @param  lit true 点亮, false 熄灭
     * @return 设置成功返回 true, 设备未绑定或 ioctl 失败返回 false
     */
    bool ApplyLit(bool lit);
    /**
     * @brief 翻转一次: 周期任务的单步动作, 两后端分支共用
     */
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
