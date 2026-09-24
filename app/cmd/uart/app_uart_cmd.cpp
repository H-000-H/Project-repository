/**
 * @file app_uart_cmd.cpp
 * @author H-000-H
 * @brief 命令层实现: 命令注册 + UART 收包回调 → SystemCmd 分发
 * @note  分层方向是"业务依赖通信": UartCommunicate 不认识任何命令/业务类型,
 *        回调由本层通过 set_rx_callback() 挂上去。
 * @note  行协议: 命令以 '\n' 或 '\r' 结束(如 "OFF\n"); 串口可能把一行拆成多次回调送达
 *        (拆包), 也可能一次送达多行(粘包), 故按字节累积成帧后再分发。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "app_uart_cmd.hpp"

#include <cstdint>
#include <cstring>

#include "app_uart_recv.hpp"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    namespace
    {
        // 日志标签: 只有本文件用, 故不进头文件
        constexpr const char* k_tag = "cmd";

        constexpr std::size_t k_line_max = CommunicateCore::k_buffer_size;

        // 串口行命令 "cmdota <len>" 的前缀与长度
        constexpr const char* k_ota_cmd_prefix     = "cmdota ";
        constexpr std::size_t k_ota_cmd_prefix_len = 7u;

        // 行累积缓冲 (拆包/粘包都靠它兜住)
        char        s_line_buf[k_line_max];
        std::size_t s_line_len = 0u;

        /**
         * @brief 十进制解析: 只收纯数字
         * @param[in]  s   const char*  待解析字符串起始 (不要求以 '\0' 结尾)
         * @param[in]  n   std::size_t  参与解析的字符数
         * @param[out] out std::uint32_t* 解析成功时写入结果
         * @return bool 空串/非数字/溢出/零 一律返回 false
         */
        bool parse_dec(const char* s, std::size_t n, std::uint32_t* out)
        {
            if ((s == nullptr) || (out == nullptr) || (n == 0u))
            {
                return false;
            }

            std::uint32_t value = 0u;
            for (std::size_t i = 0u; i < n; i++)
            {
                const char c = s[i];
                if ((c < '0') || (c > '9'))
                {
                    return false;
                }

                const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
                if (value > ((UINT32_MAX - digit) / 10u)) // 溢出保护
                {
                    return false;
                }
                value = (value * 10u) + digit;
            }

            if (value == 0u)
            {
                return false;
            }

            *out = value;
            return true;
        }

        /**
         * @brief 分发 led.set 命令: 打包 LedArgs 后交给 SystemCmd 安全分发
         * @param[in] mode LedMode LED 控制模式 (ON / OFF / AUTO)
         */
        void dispatch_led_set(LedMode mode)
        {
            const LedArgs args{mode};
            const int     ret = SystemCmd::get_instance().dispatch_secure<LedArgs>(k_led_set_command, args);
            if (ret != MINI_OK)
            {
                MT_LOG_ERROR(k_tag, "dispatch %s failed: %d", k_led_set_command, ret);
            }
        }

        /**
         * @brief 分发 ota.start 命令: 打包 OtaArgs 后交给 SystemCmd 安全分发
         * @param[in] len std::uint32_t 固件镜像总字节数
         */
        void dispatch_ota_start(std::uint32_t len)
        {
            const OtaArgs args{len};
            const int     ret = SystemCmd::get_instance().dispatch_secure<OtaArgs>(k_ota_start_command, args);
            if (ret != MINI_OK)
            {
                MT_LOG_ERROR(k_tag, "dispatch %s failed: %d", k_ota_start_command, ret);
            }
        }

        /**
         * @brief 处理一行命令: 精确匹配 (不做 trim), 分发 LED 控制或 OTA 启动
         * @param[in] line const char* 行缓冲起始 (不含结束符)
         * @param[in] len  std::size_t 行长度 (字节)
         */
        void handle_line(const char* line, std::size_t len)
        {
            if (len == 0u)
            {
                return;
            }

            if ((len == 2u) && (memcmp(line, "ON", 2u) == 0))
            {
                dispatch_led_set(LedMode::ON);
            }
            else if ((len == 3u) && (memcmp(line, "OFF", 3u) == 0))
            {
                dispatch_led_set(LedMode::OFF);
            }
            else if ((len == 4u) && (memcmp(line, "AUTO", 4u) == 0))
            {
                dispatch_led_set(LedMode::AUTO);
            }
            // cmdota <len>: 启动 OTA 并告知镜像总长度 (设备据此定位镜像尾部 meta)
            else if ((len > k_ota_cmd_prefix_len) && (memcmp(line, k_ota_cmd_prefix, k_ota_cmd_prefix_len) == 0))
            {
                std::uint32_t fw_len = 0u;
                if (parse_dec(&line[k_ota_cmd_prefix_len], len - k_ota_cmd_prefix_len, &fw_len))
                {
                    dispatch_ota_start(fw_len);
                }
                else
                {
                    MT_LOG_WARN(k_tag, "cmdota: bad length (usage: cmdota <bytes>)");
                }
            }
            else
            {
                MT_LOG_WARN(k_tag, "unknown cmd, len=%u", static_cast<unsigned>(len));
            }
        }
    } // namespace

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // UART 收包回调: 逐字节累积, 遇 '\n'/'\r' 成帧后分发 (可处理拆包/粘包)
    void Cmd::on_uart_rx(const std::uint8_t* data, std::size_t len)
    {
        if ((data == nullptr) || (len == 0u))
        {
            return;
        }

        for (std::size_t i = 0u; i < len; i++)
        {
            const char ch = static_cast<char>(data[i]);

            if ((ch == '\n') || (ch == '\r')) // 帧结束 (CR/LF/CRLF 都收)
            {
                if (s_line_len > 0u)
                {
                    handle_line(s_line_buf, s_line_len);
                    s_line_len = 0u;
                }
                continue;
            }

            if (s_line_len >= k_line_max) // 行过长: 丢弃整行并复位, 免得状态卡死
            {
                MT_LOG_WARN(k_tag, "line too long, dropped");
                s_line_len = 0u;
            }
            s_line_buf[s_line_len++] = ch;
        }
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 命令层初始化: 注册 led.set / ota.start 命令, 并把 UART 收包回调挂到实例
    void Cmd::init()
    {
        const int ret = SystemCmd::get_instance().register_cmd<LedArgs>(
            k_led_set_command,
            [](const LedArgs& arg, void*) -> bool
            {
                Led& led = Led::get_instance();
                switch (arg.mode)
                {
                case LedMode::ON:
                    return led.turn_on();
                case LedMode::AUTO:
                    return led.resume_blink();
                case LedMode::OFF:
                default:
                    return led.turn_off();
                }
            });
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "register %s failed: %d", k_led_set_command, ret);
        }

        const int ret_ota = SystemCmd::get_instance().register_cmd<OtaArgs>(
            k_ota_start_command,
            [](const OtaArgs& arg, void*) -> bool
            {
                if (arg.len == 0u) // 0 长度会让 ota_step 直接返回, 提前拦掉免得白等
                {
                    MT_LOG_WARN(k_tag, "ota.start: zero length rejected");
                    return false;
                }
                Ota::get_instance().request_ota(arg.len);
                MT_LOG_INFO(k_tag, "ota requested, fw_len=%u, waiting for image on ota uart",
                            static_cast<unsigned>(arg.len));
                return true;
            });
        if (ret_ota != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "register %s failed: %d", k_ota_start_command, ret_ota);
        }

        UartCommunicate::get_instance().set_rx_callback(on_uart_rx);
        MT_LOG_INFO(k_tag, "cmd layer ready");
    }

} // namespace app
