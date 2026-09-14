/**
 *@file led.hpp
 *@brief LED 周期任务实现
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@author H-000-H
 */
#pragma once
#include "xtask.h"
#include <cstdint>

// 前向声明 C 结构体
struct device;

namespace App_Led
{
    class Led
    {
    public:
        static Led& get_instance();

        // 禁止拷贝和移动
        Led(const Led&) = delete;
        Led& operator=(const Led&) = delete;
        Led(Led&&) = delete;
        Led& operator=(Led&&) = delete;
        /** @brief 手动控制: 点亮/熄灭, 并接管 LED(停止周期翻转) */
        bool set_light(bool on);
        /** @brief 交还控制权: 周期任务恢复翻转 */
        bool set_auto();
        void thread(x_task* self);   // 协程回调：翻转 LED
        bool thread_register(void);  // 注册任务到调度器

    private:
        Led();

        ::device* m_dev = nullptr;
        /** @brief 命令接管标志: true 后周期任务停止翻转, 避免把 set_light() 的结果覆盖掉 */
        bool m_manual = false;
        static constexpr unsigned int kDelay = 500;
        static constexpr unsigned int kPriority = 5;
        static constexpr const char* kName = "Led_Task";
        static constexpr const char* kTag = "Led";

#ifndef CONFIG_XTASK_PREEMPT
        static x_task s_tcb;
#endif
    };

} // namespace App_Led
