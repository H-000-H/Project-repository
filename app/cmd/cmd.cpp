/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file cmd.cpp
 * @brief 命令层实现: 命令注册 + UART 收包回调 → SystemCmd 分发
 * @author H-000-H
 * @note  分层方向是"业务依赖通信": UartCommunicate 不认识任何命令/业务类型,
 *        回调由本层通过 SetRxHandler() 挂上去。
 * @note  行协议: 命令以 '\n' 或 '\r' 结束(如 "OFF\n"); 串口可能把一行拆成多次
 *        回调送达(拆包), 也可能一次送达多行(粘包), 故按字节累积成帧后再分发。
 */
#include "cmd.hpp"

#include <cstring>

#include "communicate_uart.hpp"

namespace app_cmd
{

bool HandleLedSet(const LedArgs& arg, app_led::Led* ctx)
{
    if (ctx == nullptr)
        return false;

    switch (arg.mode)
    {
    case LedMode::kOn:
        return ctx->TurnOn();
    case LedMode::kAuto:
        return ctx->ResumeBlink();
    case LedMode::kOff:
    default:
        return ctx->TurnOff();
    }
}

namespace
{

constexpr std::size_t kLineMax = app_communicate::kBufferSize;

char        line_buf[kLineMax];
std::size_t line_len = 0u;

void DispatchLedSet(LedMode mode)
{
    const LedArgs args{mode};
    const int     ret = SystemCmd::get_instance().dispatch_secure<LedArgs, app_led::Led>
    (kLedSetCommand, args, &app_led::Led::GetInstance());
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "dispatch %s failed: %d", kLedSetCommand, ret);
    }
}

/* 精确匹配, 不做 trim */
void HandleLine(const char* line, std::size_t len)
{
    if (len == 0u)
        return;

    if ((len == 2u) && (memcmp(line, "ON", 2u) == 0))
    {
        DispatchLedSet(LedMode::kOn);
    }
    else if ((len == 3u) && (memcmp(line, "OFF", 3u) == 0))
    {
        DispatchLedSet(LedMode::kOff);
    }
    else if ((len == 4u) && (memcmp(line, "AUTO", 4u) == 0))
    {
        DispatchLedSet(LedMode::kAuto);
    }
    else
    {
        MT_LOG_WARN(kTag, "unknown cmd, len=%u", (unsigned)len);
    }
}

} // namespace

void OnUartRx(const uint8_t* data, size_t len)
{
    if ((data == nullptr) || (len == 0u))
        return;

    for (size_t i = 0u; i < len; i++)
    {
        const char ch = static_cast<char>(data[i]);

        if ((ch == '\n') || (ch == '\r')) /* 帧结束 (CR/LF/CRLF 都收) */
        {
            if (line_len > 0u)
            {
                HandleLine(line_buf, line_len);
                line_len = 0u;
            }
            continue;
        }

        if (line_len >= kLineMax) /* 行过长: 丢弃整行并复位, 免得状态卡死 */
        {
            MT_LOG_WARN(kTag, "line too long, dropped");
            line_len = 0u;
        }
        line_buf[line_len++] = ch;
    }
}

void Init()
{
    const int ret = SystemCmd::get_instance().register_cmd<LedArgs, app_led::Led>
    (    kLedSetCommand, HandleLedSet);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "register %s failed: %d", kLedSetCommand, ret);
    }

    app_communicate::UartCommunicate::GetInstance().SetRxHandler(OnUartRx);
    MT_LOG_INFO(kTag, "cmd layer ready");
}

} // namespace app_cmd
