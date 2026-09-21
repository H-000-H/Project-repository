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

constexpr const char* k_led_set_command   = "led.set";   /* 裸机后端要求静态字面量 */
constexpr const char* k_ota_start_command = "ota.start"; /* 裸机后端要求静态字面量 */

/** @brief LED 控制模式 (串口行命令: ON / OFF / AUTO) */
enum class LedMode : uint8_t
{
    OFF  = 0u, /**< 熄灭并接管 */
    ON   = 1u, /**< 点亮并接管 */
    AUTO = 2u, /**< 交还控制权, 恢复周期翻转 */
};

/** @brief led.set 命令参数 */
struct LedArgs
{
    LedMode mode; /**< LED 控制模式 */
};

/** @brief ota.start 命令参数 (串口行命令: cmdota <len>) */
struct OtaArgs
{
    /** 固件镜像总字节数: mini-ota 靠它定位镜像尾部 meta, 必须与随后从 OTA 口灌入的
     *  字节数严格一致, 多一字节少一字节都会让校验失败 */
    uint32_t len;
};

/** @brief 命令层: 注册 led.set / ota.start 命令, 并把 UART 收包回调挂到实例 */
class Cmd
{
public:
    /** @brief 命令层初始化: 注册 led.set / ota.start 命令, 并把 UART 收包回调挂到实例 */
    static void init();

private:
    /**
     * @brief UART 收包回调: 逐字节累积, 遇 '\n'/'\r' 成帧后分发 (可处理拆包/粘包)
     * @param[in] data const uint8_t* 收到的字节缓冲
     * @param[in] len  size_t         字节数
     */
    static void on_uart_rx(const uint8_t* data, size_t len);
};

} // namespace app

#endif // APP_CMD_UART_APP_UART_CMD_HPP_
