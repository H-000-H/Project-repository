/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_uart_cmd.cpp
 * @brief 命令层实现: 命令注册 + UART 收包回调 → SystemCmd 分发
 * @author H-000-H
 * @note  分层方向是"业务依赖通信": UartCommunicate 不认识任何命令/业务类型,
 *        回调由本层通过 SetRxCallback() 挂上去。
 * @note  行协议: 命令以 '\n' 或 '\r' 结束(如 "OFF\n"); 串口可能把一行拆成多次
 *        回调送达(拆包), 也可能一次送达多行(粘包), 故按字节累积成帧后再分发。
 */
#include "app_uart_cmd.hpp"

#include <cstring>

#include "app_uart_recv.hpp"

namespace app
{

namespace
{

constexpr const char* kTag     = "cmd"; /* 日志标签: 只有本文件用, 故不进头文件 */
constexpr std::size_t kLineMax = CommunicateCore::kBufferSize;

/* 串口行命令 "cmdota <len>" 的前缀与长度 */
constexpr const char* kOtaCmdPrefix    = "cmdota ";
constexpr std::size_t kOtaCmdPrefixLen = 7u;

char        line_buf[kLineMax];
std::size_t line_len = 0u;

/**
 * @brief  十进制解析: 只收纯数字, 空串/非数字/溢出/零 一律失败
 * @param  s   待解析字符串起始 (不要求以 '\0' 结尾)
 * @param  n   参与解析的字符数
 * @param  out 解析成功时写入结果
 * @return 解析成功返回 true, 否则返回 false
 */
bool ParseDec(const char* s, std::size_t n, uint32_t* out)
{
    if ((s == nullptr) || (out == nullptr) || (n == 0u))
        return false;

    uint32_t value = 0u;
    for (std::size_t i = 0u; i < n; i++)
    {
        const char c = s[i];
        if ((c < '0') || (c > '9'))
            return false;

        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (value > ((UINT32_MAX - digit) / 10u)) /* 溢出保护 */
            return false;
        value = (value * 10u) + digit;
    }

    if (value == 0u)
        return false;

    *out = value;
    return true;
}

/**
 * @brief 分发 led.set 命令: 打包 LedArgs 后交给 SystemCmd 安全分发
 * @param mode LED 控制模式 (ON / OFF / AUTO)
 */
void DispatchLedSet(LedMode mode)
{
    const LedArgs args{mode};
    const int     ret  = SystemCmd::get_instance().dispatch_secure<LedArgs>(kLedSetCommand, args);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "dispatch %s failed: %d", kLedSetCommand, ret);
    }
}

/**
 * @brief 分发 ota.start 命令: 打包 OtaArgs 后交给 SystemCmd 安全分发
 * @param len 固件镜像总字节数
 */
void DispatchOtaStart(uint32_t len)
{
    const OtaArgs args{len};
    const int     ret  = SystemCmd::get_instance().dispatch_secure<OtaArgs>(kOtaStartCommand, args);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "dispatch %s failed: %d", kOtaStartCommand, ret);
    }
}

/**
 * @brief 处理一行命令: 精确匹配 (不做 trim), 分发 LED 控制或 OTA 启动
 * @param line 行缓冲起始 (不含结束符)
 * @param len  行长度 (字节)
 */
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
    /* cmdota <len>: 启动 OTA 并告知镜像总长度 (设备据此定位镜像尾部 meta) */
    else if ((len > kOtaCmdPrefixLen) && (memcmp(line, kOtaCmdPrefix, kOtaCmdPrefixLen) == 0))
    {
        uint32_t fw_len = 0u;
        if (ParseDec(&line[kOtaCmdPrefixLen], len - kOtaCmdPrefixLen, &fw_len))
        {
            DispatchOtaStart(fw_len);
        }
        else
        {
            MT_LOG_WARN(kTag, "cmdota: bad length (usage: cmdota <bytes>)");
        }
    }
    else
    {
        MT_LOG_WARN(kTag, "unknown cmd, len=%u", (unsigned)len);
    }
}

} // namespace

/**
 * @brief UART 收包回调: 逐字节累积, 遇 '\n'/'\r' 成帧后分发 (可处理拆包/粘包)
 * @param data 收到的字节缓冲
 * @param len  字节数
 */
void Cmd::OnUartRx(const uint8_t* data, size_t len)
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

/**
 * @brief 命令层初始化: 注册 led.set / ota.start 命令, 并把 UART 收包回调挂到实例
 */
void Cmd::Init()
{
    const int ret = SystemCmd::get_instance().register_cmd<LedArgs>(
        kLedSetCommand,
        /*param[in] 参数 param[in]上下文*/
        [](const LedArgs& arg, void*) -> bool
        {
            Led& led = Led::GetInstance();
            switch (arg.mode)
            {
            case LedMode::kOn:
                return led.TurnOn();
            case LedMode::kAuto:
                return led.ResumeBlink();
            case LedMode::kOff:
            default:
                return led.TurnOff();
            }
        });
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "register %s failed: %d", kLedSetCommand, ret);
    }

    const int ret_ota = SystemCmd::get_instance().register_cmd<OtaArgs>(
        kOtaStartCommand,
        [](const OtaArgs& arg, void*) -> bool
        {
            if (arg.len == 0u) /* 0 长度会让 OtaStep 直接返回, 提前拦掉免得白等 */
            {
                MT_LOG_WARN(kTag, "ota.start: zero length rejected");
                return false;
            }
            Ota::GetInstance().RequestOta(arg.len); /* 内部 SetFwLen + ota_open + ota_rollback_open + 置启动标志 */
            MT_LOG_INFO(kTag, "ota requested, fw_len=%u, waiting for image on ota uart", (unsigned)arg.len);
            return true;
        });
    if (ret_ota != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "register %s failed: %d", kOtaStartCommand, ret_ota);
    }

    UartCommunicate::GetInstance().SetRxCallback(OnUartRx);
    MT_LOG_INFO(kTag, "cmd layer ready");
}

} // namespace app
