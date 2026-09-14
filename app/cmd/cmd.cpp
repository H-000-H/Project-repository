/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file cmd.cpp
 * @brief 命令层实现: 命令注册 + UART 收包回调 → SystemCmd 分发
 * @author H-000-H
 * @note  分层方向是"业务依赖通信": UartCommunicate 不认识任何命令/业务类型,
 *        回调由本层通过 set_rx_handler() 挂上去。
 * @note  协议为行协议: 命令以 '\n' 或 '\r' 结束, 如 "OFF\n"。
 *        串口可能把一行拆成多次回调送达(拆包), 也可能一次送达多行(粘包),
 *        所以这里必须按字节累积到分隔符才成帧。
 */
#include "cmd.hpp"
#include "communicate_uart.hpp"

#include <cstring>

namespace App_Cmd
{
    namespace
    {
        /* 累积缓冲上限取通信层单次接收上限: 协议行不可能比一次读到的数据更长 */
        constexpr std::size_t kLineMax = APP_Communicate::kBufferSize;
        char s_line[kLineMax];
        std::size_t s_line_len = 0u;

        void dispatch_led(LedMode mode)
        {
            const LedArgs args{static_cast<uint8_t>(mode)};
            const int ret = SystemCmd::get_instance().dispatch_secure<LedArgs, App_Led::Led>(
                k_led_set_cmd, args, &App_Led::Led::get_instance());
            if (ret != MINI_OK)
            {
                MT_LOG_ERROR(kTag, "dispatch %s failed: %d", k_led_set_cmd, ret);
            }
        }

        /* 一帧完整命令: 这里只做精确匹配, 不做 trim */
        void handle_line(const char* line, std::size_t len)
        {
            if (len == 0u)
                return;

            if ((len == 2u) && (memcmp(line, "ON", 2u) == 0))
            {
                dispatch_led(k_led_on);
            }
            else if ((len == 3u) && (memcmp(line, "OFF", 3u) == 0))
            {
                dispatch_led(k_led_off);
            }
            else if ((len == 4u) && (memcmp(line, "AUTO", 4u) == 0))
            {
                dispatch_led(k_led_auto);
            }
            else
            {
                MT_LOG_WARN(kTag, "unknown cmd, len=%u", (unsigned)len);
            }
        }
    } // namespace

    void on_uart_rx(const uint8_t* data, size_t len)
    {
        if ((data == nullptr) || (len == 0u))
            return;

        for (size_t i = 0u; i < len; i++)
        {
            const char ch = static_cast<char>(data[i]);

            if ((ch == '\n') || (ch == '\r'))   /* 帧结束(CR/LF/CRLF 都收) */
            {
                if (s_line_len > 0u)
                {
                    handle_line(s_line, s_line_len);
                    s_line_len = 0u;
                }
                continue;
            }

            if (s_line_len >= kLineMax)         /* 行过长: 丢弃整行并复位 */
            {
                MT_LOG_WARN(kTag, "line too long, dropped");
                s_line_len = 0u;
            }
            s_line[s_line_len++] = ch;
        }
    }

    void init()
    {
        const int ret = SystemCmd::get_instance().register_cmd<LedArgs, App_Led::Led>(
            k_led_set_cmd, led_set_handler);
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(kTag, "register %s failed: %d", k_led_set_cmd, ret);
        }

        APP_Communicate::UartCommunicate::getInstance().set_rx_handler(on_uart_rx);
        MT_LOG_INFO(kTag, "cmd layer ready");
    }
} // namespace App_Cmd
