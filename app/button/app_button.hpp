/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_button.hpp
 * @brief 板载按键: 具体按键子类 + 唯一的扫描驱动任务 (仅声明)
 * @author H-000-H
 * @details 基类(持有库对象/绑定 pdev/注册回调)在 app/button/button_base/app_button_base.hpp;
 *          本目录只放"每颗按键自己的数据 + 回调 + 扫描任务", 子类实现见 .cpp
 */
#ifndef APP_BUTTON_HPP
#define APP_BUTTON_HPP

#include "app_button_base.hpp"
#include <cstdint>

namespace app
{
/** @brief 按键模块: 唯一的扫描驱动任务 + 触发电平约定 */
class Button
{
public:
    /** @brief 采样钩子: 把上层桥接的动作注进扫描任务 (无堆; 本模块因此不认识 LVGL) */
    using HookFn = void (*)(void* ctx);

    /**
     * @brief 注入采样钩子: 任务起来时调 open 一次, 之后每周期调 sample 一次
     * @param[in] open   HookFn 打开上层桥接用 (可为空)
     * @param[in] sample HookFn 采样上层那颗按键 (可为空)
     * @param[in] ctx    void*  透传给两个钩子的上下文 (类型由装配方与钩子约定)
     * @note  必须在 thread_register 之前调
     */
    static void set_sample_hook(HookFn open, HookFn sample, void* ctx);

    /**
     * @brief 注册扫描任务 (main 里调用一次)
     * @return bool 创建成功返回 true, 失败返回 false
     */
    static bool thread_register();

private:
    /**
     * @brief 扫描任务体: 先开上层桥接, 再死循环(采样 + 推进状态机)
     * @param[in] param void* 线程参数 (未使用)
     */
    static void thread(void* param);

    static HookFn s_open;   /**< 任务启动时调一次 */
    static HookFn s_sample; /**< 每周期调一次 */
    static void*  s_ctx;    /**< 钩子上下文 */

    static constexpr std::uint16_t k_stack_size  = 2048;       /**< mini-os 线程栈 (字节) */
    static constexpr const char*   k_thread_name = "ButtonTask";
    static constexpr std::uint8_t  k_priority    = 26;         /**< mini-os: 数值越小越优先 */
};

} // namespace app

#endif // APP_BUTTON_HPP
