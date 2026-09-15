/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_uart_cmd.hpp
 * @brief 命令层: led.set / ota.start 命令定义 + UART 收包回调入口
 * @author H-000-H
 */
#ifndef APP_CMD_UART_APP_UART_CMD_HPP_
#define APP_CMD_UART_APP_UART_CMD_HPP_

#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "app_led.hpp"
#include "app_ota.hpp"
#include "system_cmd.hpp"
#include "system_log.h"

namespace app_cmd
{

constexpr const char* kLedSetCommand   = "led.set";   /* 裸机后端要求静态字面量 */
constexpr const char* kOtaStartCommand = "ota.start"; /* 裸机后端要求静态字面量 */
constexpr const char* kTag             = "cmd";

/** @brief LED 控制模式 (串口行命令: ON / OFF / AUTO) */
enum class LedMode : uint8_t
{
    kOff  = 0u, /**< 熄灭并接管 */
    kOn   = 1u, /**< 点亮并接管 */
    kAuto = 2u, /**< 交还控制权, 恢复周期翻转 */
};

struct LedArgs
{
    LedMode mode;
};

/** @brief OTA 启动参数 (串口行命令: cmdota <len>) */
struct OtaArgs
{
    /** 固件镜像总字节数: mini-ota 靠它定位镜像尾部 meta, 必须与随后从 OTA 口
     *  灌入的字节数严格一致, 多一字节少一字节都会让校验失败 */
    uint32_t len;
};

bool HandleLedSet(const LedArgs& arg, app_led::Led* ctx);

/** @brief ota.start 处理: 交 Ota 任务开始接收镜像 (SetFwLen + StartOta) */
bool HandleOtaStart(const OtaArgs& arg, app_ota::Ota* ctx);

/** @brief UART 收包回调: 逐字节累积, 遇 '\n'/'\r' 成帧后分发 (可处理拆包/粘包) */
void OnUartRx(const uint8_t* data, size_t len);

/** @brief 注册 led.set / ota.start 命令, 并把 OnUartRx 挂到 UART 实例 */
void Init();

} // namespace app_cmd

#endif // APP_CMD_UART_APP_UART_CMD_HPP_
