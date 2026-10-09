/**
 * @file app_communicate_base.cpp
 * @author H-000-H
 * @brief 通用通信底座实现 (只依赖 device 的 read/write)
 * @note  只实现 CommunicateCore, 不含单例/任务 (那部分由头文件的 CRTP 基类按派生类生成);
 *        也不 include 任何总线专有头, 换设备不用改这里。接口说明见 app_communicate_base.hpp。
 * @copyright SPDX-License-Identifier: Apache-2.0
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
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    CommunicateCore::CommunicateCore(const char* label)
    {
        ::device* pdev = device_find_by_label(label);
        if (pdev == nullptr)
        {
            MT_LOG_ERROR(k_tag, "device '%s' not found", label);
            this->m_dev = nullptr;
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "device_open('%s') failed", label);
            this->m_dev = nullptr;
            return;
        }
        MT_LOG_INFO(k_tag, "device '%s' opened", label);
        this->m_dev = pdev;
    }

    // 虚析构: 默认实现
    CommunicateCore::~CommunicateCore() = default;

    void CommunicateCore::set_rx_callback(RxCallback callback)
    {
        this->m_rx_callback = callback;
    }

    std::size_t CommunicateCore::get_rx_len() const
    {
        return this->m_rx_len;
    }

    etl::span<const std::uint8_t> CommunicateCore::get_rx() const
    {
        return etl::span<const std::uint8_t>(this->m_recv_buffer.data(), this->m_rx_len);
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    etl::optional<mt_err_t> CommunicateCore::send(etl::span<const std::uint8_t> data_view, std::uint32_t time_out)
    {
        const std::size_t len = data_view.size();
        if ((len == 0) || (len > k_buffer_size)) // 动态 span: 长度由调用方给, 这里卡上限
        {
            MT_LOG_ERROR(k_tag, "invalid send length: %u (max %u)",
                         static_cast<unsigned>(len), static_cast<unsigned>(k_buffer_size));
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
        }
        if (this->m_dev == nullptr) // 构造时已报过, 静默返回免得调用方反复触发
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = k_default_timeout;
        }
        etl::copy(data_view.begin(), data_view.end(), this->m_send_buffer.begin());

        const mt_err_t ret = device_write(this->m_dev, this->m_send_buffer.data(), len, time_out);
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "device_write failed: %d", static_cast<int>(ret));
        }
        return etl::make_optional(ret);
    }

    etl::optional<mt_err_t> CommunicateCore::receive(std::uint32_t time_out)
    {
        if (this->m_dev == nullptr) // 每任务周期调一次, 打日志会刷屏
        {
            this->m_rx_len = 0u;
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = k_default_timeout;
        }
        const int n = device_read(this->m_dev, this->m_recv_buffer.data(), this->m_recv_buffer.size(), time_out);

        this->m_rx_len = (n > 0) ? static_cast<std::size_t>(n) : 0u;

        if (n > 0)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
        }
        if (n == MINI_ERR_TIMEOUT) // 零字节超时属正常轮询结果
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));
        }
        MT_LOG_ERROR(k_tag, "device_read failed: %d", n);
        return etl::make_optional(static_cast<mt_err_t>(n));
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    int CommunicateCore::poll_once(std::uint32_t time_out)
    {
        (void)this->receive(time_out); // receive 内部已置 m_rx_len 并处理错误日志

        if (this->m_rx_len == 0u)
        {
            return 0;
        }
        if (this->m_rx_callback != nullptr)
        {
            this->m_rx_callback(this->m_recv_buffer.data(), this->m_rx_len);
        }
        else
        {
            this->recv_log();
        }
        return static_cast<int>(this->m_rx_len);
    }

    etl::optional<mt_err_t> CommunicateCore::send_resv(etl::span<const std::uint8_t> data_view, std::uint32_t time_out)
    {
        (void)data_view;
        (void)time_out;
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NOTSUPP));
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void CommunicateCore::send_log()
    {
        MT_LOG_INFO(k_tag, "Send: %s", this->m_send_buffer.data());
    }

    void CommunicateCore::recv_log()
    {
        MT_LOG_INFO(k_tag, "Receive: %s", this->m_recv_buffer.data());
    }

    void CommunicateCore::send_recv_log()
    {
        MT_LOG_INFO(k_tag, "Send: %s, Receive: %s", this->m_send_buffer.data(), this->m_recv_buffer.data());
    }

} // namespace app
