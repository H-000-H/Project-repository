#pragma once
#include <cstdint>
#include <cstddef>
#include "system_cmd.hpp"
#include "led.hpp"
#include "system_log.h"
namespace App_Cmd
{
    /* 命令名/日志标签收一处: 裸机后端要求命令名是静态字符串字面量 */
    constexpr const char* k_led_set_cmd = "led.set";
    constexpr const char* kTag = "cmd";

    /** @brief LED 控制模式 (串口行命令: ON / OFF / AUTO) */
    enum LedMode : uint8_t
    {
        k_led_off = 0u,
        k_led_on = 1u,
        k_led_auto = 2u,
    };

    struct LedArgs
    {
        uint8_t mode; // 取值见 LedMode
    };

    inline bool led_set_handler(const LedArgs& arg, App_Led::Led* ctx)
    {
        if (ctx == nullptr)
            return false;
        switch (arg.mode)
        {
        case k_led_on:
            return ctx->set_light(true);
        case k_led_auto:
            return ctx->set_auto();
        case k_led_off:
        default:
            return ctx->set_light(false);
        }
    }

    /** @brief 收到 UART 数据时的业务回调(签名同 CommunicateCore::rx_handler_t)
     *  @note  按字节累积, 遇 '\n' / '\r' 才成帧后分发, 因此能处理拆包/粘包 */
    void on_uart_rx(const uint8_t* data, size_t len);

    /** @brief 注册 led.set 命令, 并把 on_uart_rx 挂到 UART 通信实例上 */
    void init();
} // namespace App_Cmd
