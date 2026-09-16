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

#include "app_led.hpp"
#include "app_ota.hpp"
#include "system_cmd.hpp"
#include "system_log.h"

namespace app
{

constexpr const char* kLedSetCommand   = "led.set";   /* 裸机后端要求静态字面量 */
constexpr const char* kOtaStartCommand = "ota.start"; /* 裸机后端要求静态字面量 */

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

/** @brief 命令层入口: 注册 led.set / ota.start 命令, 并把 UART 收包回调挂到实例 */
class Cmd
{
public:
    static void Init();

private:
    /** @brief UART 收包回调: 逐字节累积, 遇 '\n'/'\r' 成帧后分发 (可处理拆包/粘包) */
    static void OnUartRx(const uint8_t* data, size_t len);
};

} // namespace app

#endif // APP_CMD_UART_APP_UART_CMD_HPP_
