/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_button.hpp
 * @brief 板载按键: 具体按键子类 + 唯一的扫描驱动任务 (仅声明)
 * @author H-000-H
 * @details 基类(持有库对象/绑定 pdev/注册回调)在 app/button_base/app_button_base.hpp;
 *          本目录只放"每颗按键自己的数据 + 回调 + 扫描任务", 子类实现见 .cpp。
 */
#ifndef APP_BUTTON_HPP
#define APP_BUTTON_HPP

#include "app_button_base.hpp"
#include <atomic>
#include <cstdint>

namespace app
{
/**
 * @brief 按键模块: 唯一的扫描驱动任务 + 触发电平约定
 * @note 具体按键子类 (AppButton1 / AppButton2) 在本文件的 .cpp 里, 只写回调。
 */
class Button
{
public:
    /**
     * @brief  注册扫描任务 (main 里调用一次)
     * @return 创建成功返回 true, 失败返回 false
     */
    static bool ThreadRegister();

private:
    /**
     * @brief 扫描任务体: 初始化 LVGL 桥接后死循环采样并驱动按键状态机
     * @param param 线程参数 (未使用)
     */
    static void Thread(void* param);

    static constexpr std::uint16_t kStackSize  = 2048;        /**< mini-os 线程栈 (字节) */
    static constexpr const char*   kThreadName = "ButtonTask";
    static constexpr std::uint8_t  kPriority   = 26;          /**< mini-os: 数值越小越优先 (扫描最不急) */
};

} // namespace app

#endif // APP_BUTTON_HPP
