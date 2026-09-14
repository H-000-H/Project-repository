/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_config.hpp
 * @brief 应用层"随后端变化的数值"集中处 (只放数值, 不放宏 / 封装函数)
 * @author H-000-H
 * @details 各模块的任务创建与循环体一律直接写各后端的**原生语句**, 用 #if 显式分支
 *          (裸机 xtask: xscheduler_task_create / PT_BEGIN / PT_DELAY;
 *           真线程后端: mini_task_create_handle / 内核毫秒延时)。
 *          本文件只回答一个问题: 哪些量会因为换后端而取不同的值。
 *
 *            - 任务优先级: 裸机抢占式与 FreeRTOS 是"数值越大越优先",
 *                          mini-os 与 RT-Thread 是"数值越小越优先" —— 语义相反,
 *                          同一个任务在两组后端下必须给不同数值。
 *            - 任务栈大小: 只有真线程后端有意义 (裸机 xtask 是协程, 共用调用栈,
 *                          该值被忽略), 且换内核后要重测水位。
 *
 *          与后端无关的值 —— 任务名 / 周期 / 日志标签 / 设备名 / 缓冲大小 / 超时 ——
 *          **留在各自模块里**, 不往本文件搬。
 */
#ifndef APP_APP_CONFIG_HPP_
#define APP_APP_CONFIG_HPP_

#include <cstdint>

namespace app_config
{

/* -------------------------------------------------------------------------- */
/* 任务优先级                                                                  */
/* -------------------------------------------------------------------------- */
/* 语义随后端相反, 故按后端给两组值; 轻重关系: OTA(最不急) < 通信 = LED */
#if defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_RTTHREAD)
/* mini-os / RT-Thread: 数值越小越优先 */
constexpr unsigned int kLedTaskPriority         = 12;
constexpr unsigned int kOtaTaskPriority         = 14;
constexpr unsigned int kCommunicateTaskPriority = 12;
#else
/* 裸机抢占式 / FreeRTOS: 数值越大越优先 */
constexpr unsigned int kLedTaskPriority         = 5;
constexpr unsigned int kOtaTaskPriority         = 4;
constexpr unsigned int kCommunicateTaskPriority = 5;
#endif

/* -------------------------------------------------------------------------- */
/* 任务栈大小 (字节)                                                           */
/* -------------------------------------------------------------------------- */
/* 仅真线程后端使用; 裸机 xtask 是协程, 共用调用栈, 该值被忽略 */
constexpr std::uint32_t kLedTaskStack         = 2048;
constexpr std::uint32_t kOtaTaskStack         = 1024;
constexpr std::uint32_t kCommunicateTaskStack = 1024;

} // namespace app_config

#endif // APP_APP_CONFIG_HPP_
