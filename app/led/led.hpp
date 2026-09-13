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

        void thread(x_task* self);   // 协程回调：翻转 LED
        bool thread_register(void);  // 注册任务到调度器

    private:
        Led();

        ::device* m_dev = nullptr;

        static constexpr unsigned int kDelay = 500;
        static constexpr unsigned int kPriority = 5;
        static constexpr const char* kName = "Led_Task";
        static constexpr const char* kTag = "Led";

#ifndef CONFIG_XTASK_PREEMPT
        static x_task s_tcb;
#endif
    };

} // namespace App_Led
