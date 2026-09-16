/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_communicate_base.cpp
 * @brief 通用通信底座实现 (只依赖 device 的 read/write)
 * @author H-000-H
 * @note  本文件只实现 CommunicateCore, 不含单例/任务 —— 单例与协程任务由
 *        app_communicate_base.hpp 的 CRTP 基类按派生类生成。
 *        也不 include 任何总线专有头(vfs-uart.h 等), 换设备不用改这里。
 * @note  
 *        - 零字节超时返回 MINI_ERR_TIMEOUT, 属正常轮询结果, 不算错误。
 */
#include "app_communicate_base.hpp"

#include <cstdint>

#include "device.h"
#include "etl/algorithm.h"
#include "etl/optional.h"
#include "status.h"
#include "system_log.h"

namespace app
{

CommunicateCore::CommunicateCore(const char* label)
{
    ::device* pdev = device_find_by_label(label);
    if (pdev == nullptr)
    {
        MT_LOG_ERROR(kTag, "device '%s' not found", label);
        m_dev = nullptr;
        return;
    }
    if (device_open(pdev, nullptr) != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "device_open('%s') failed", label);
        m_dev = nullptr;
        return;
    }
    MT_LOG_INFO(kTag, "device '%s' opened", label);
    m_dev = pdev;
}

CommunicateCore::~CommunicateCore() = default;

void CommunicateCore::SetRxCallback(RxCallback callback)
{
    m_rx_callback = callback;
}

std::size_t CommunicateCore::GetRxLen() const
{
    return m_rx_len;
}

etl::span<const uint8_t> CommunicateCore::GetRx() const
{
    return etl::span<const uint8_t>(m_recv_buffer.data(), m_rx_len);
}

etl::optional<mt_err_t> CommunicateCore::Send(etl::span<const uint8_t> data_view,
                                              uint32_t time_out)
{
    const std::size_t len = data_view.size();
    if ((len == 0) || (len > kBufferSize)) /* 动态 span: 长度由调用方给, 这里卡上限 */
    {
        MT_LOG_ERROR(kTag, "invalid send length: %u (max %u)", (unsigned)len,
                     (unsigned)kBufferSize);
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
    }
    if (m_dev == nullptr) /* 构造时已报过, 静默返回免得调用方反复触发 */
    {
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
    }
    if (time_out == 0)
    {
        time_out = kDefaultTimeout;
    }
    etl::copy(data_view.begin(), data_view.end(), m_send_buffer.begin());

    const mt_err_t ret = device_write(m_dev, m_send_buffer.data(), len, time_out);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "device_write failed: %d", static_cast<int>(ret));
    }
    return etl::make_optional(ret);
}

etl::optional<mt_err_t> CommunicateCore::Receive(uint32_t time_out)
{
    if (m_dev == nullptr) /* 每任务周期调一次, 打日志会刷屏 */
    {
        m_rx_len = 0u;
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
    }
    if (time_out == 0)
    {
        time_out = kDefaultTimeout;
    }
    const int n = device_read(m_dev, m_recv_buffer.data(), m_recv_buffer.size(), time_out);

    m_rx_len = (n > 0) ? static_cast<std::size_t>(n) : 0u;

    if (n > 0)
    {
        return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
    }
    if (n == MINI_ERR_TIMEOUT) /* 零字节超时属正常轮询结果 */
    {
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));
    }
    MT_LOG_ERROR(kTag, "device_read failed: %d", n);
    return etl::make_optional(static_cast<mt_err_t>(n));
}

int CommunicateCore::PollOnce(uint32_t time_out)
{
    (void)Receive(time_out); /* Receive 内部已置 m_rx_len 并处理错误日志 */

    if (m_rx_len == 0u)
    {
        return 0;
    }
    if (m_rx_callback != nullptr)
    {
        m_rx_callback(m_recv_buffer.data(), m_rx_len);
    }
    else
    {
        RecvLog();
    }
    return static_cast<int>(m_rx_len);
}

/* 默认不支持: 需要"一写一读"的设备(如 UART)在派生类 override */
etl::optional<mt_err_t> CommunicateCore::SendResv(etl::span<const uint8_t> data_view,
                                                  uint32_t time_out)
{
    (void)data_view;
    (void)time_out;
    return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NOTSUPP));
}

void CommunicateCore::SendLog()
{
    MT_LOG_INFO(kTag, "Send: %s", m_send_buffer.data());
}

void CommunicateCore::RecvLog()
{
    MT_LOG_INFO(kTag, "Receive: %s", m_recv_buffer.data());
}

void CommunicateCore::SendRecvLog()
{
    MT_LOG_INFO(kTag, "Send: %s, Receive: %s", m_send_buffer.data(), m_recv_buffer.data());
}

} // namespace app
