/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file communicate.cpp
 * @brief Communication module implementation file.
 * @author H-000-H
 * @note  本文件只实现 CommunicateCore(通用读写底座), 不含单例/任务 ——
 *        单例与协程任务由 communicate.hpp 的 CRTP 基类按派生类生成。
 *        也不 include 任何总线专有头(vfs-uart.h 等), 换设备不用改这里。
 * @note  长度来源: device_read 的成功返回值就是实际读取字节数
 *        (hal_uart_read -> uart_bus_read -> uart_vfs_read -> device_read 一路透传),
 *        零字节超时返回 MINI_ERR_TIMEOUT, 属正常轮询结果, 不算错误。
 */
#include "communicate.hpp"
#include "device.h"
#include "status.h"
#include "system_log.h"
#include <cstdint>
#include <etl/optional.h>
#include <etl/algorithm.h>

namespace APP_Communicate 
{
    CommunicateCore::CommunicateCore(const char* label)
    {
        ::device* pdev = device_find_by_label(label);
        if (pdev == nullptr)
        {
            MT_LOG_ERROR(kTag, "device '%s' not found", label);
            this->pdev = nullptr;
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(kTag, "device_open('%s') failed", label);
            this->pdev = nullptr;
            return;
        }
        MT_LOG_INFO(kTag, "device '%s' opened", label);
        this->pdev = pdev;
    }

    etl::optional<mt_err_t> CommunicateCore::send(etl::span<const uint8_t> data_view, uint32_t time_out)
    {
        const std::size_t len = data_view.size();
        if ((len == 0) || (len > kBufferSize))   /* 动态 span: 长度由调用方给, 这里卡上限 */
        {
            MT_LOG_ERROR(kTag, "invalid send length: %u (max %u)", (unsigned)len, (unsigned)kBufferSize);
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
        }
        /* 设备未就绪: 构造时已打过 ERROR, 这里静默返回, 避免调用方反复触发 */
        if (pdev == nullptr)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = CommunicateCore::kDefaultTimeout;
        }
        etl::copy(data_view.begin(), data_view.end(), send_buffer.begin());

        mt_err_t ret = device_write(pdev, send_buffer.data(), len, time_out);
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(kTag, "device_write failed: %d", static_cast<int>(ret));
        }
        return etl::make_optional(ret);
    }

    etl::optional<mt_err_t> CommunicateCore::receive(uint32_t time_out)
    {
        /* 设备未就绪: 构造时已报过一次。poll_once 每 kThtreadDelay 调一次,
         * 在这里打日志会刷屏, 故静默返回 */
        if (pdev == nullptr)
        {
            rx_len = 0u;
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = CommunicateCore::kDefaultTimeout;
        }
        const int n = device_read(pdev, recv_buffer.data(), recv_buffer.size(), time_out);

        rx_len = (n > 0) ? static_cast<std::size_t>(n) : 0u;

        if (n > 0)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
        }
        /* 零字节超时: 只是超时 */
        if (n == MINI_ERR_TIMEOUT)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));
        }
        MT_LOG_ERROR(kTag, "device_read failed: %d", n);
        return etl::make_optional(static_cast<mt_err_t>(n));
    }

    int CommunicateCore::poll_once(uint32_t time_out)
    {
        (void)receive(time_out);   /* receive 内部已置 rx_len 并处理错误日志 */

        if (rx_len == 0u)
        {
            return 0;              /* 无数据(或错误): 交给下一轮 */
        }
        if (rx_handler != nullptr)
        {
            rx_handler(recv_buffer.data(), rx_len);   /* 业务回调优先 */
        }
        else
        {
            recv_log();                               /* 没注册回调就打印 */
        }
        return static_cast<int>(rx_len);
    }

    /* 默认不支持: 需要"一写一读"的设备(如 UART)在派生类 override */
    etl::optional<mt_err_t> CommunicateCore::send_resv(etl::span<const uint8_t> data_view, uint32_t time_out)
    {
        (void)data_view;
        (void)time_out;
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NOTSUPP));
    }

    void CommunicateCore::send_log()
    {
        MT_LOG_INFO(kTag, "Send: %s", send_buffer.data());
    }

    void CommunicateCore::recv_log()
    {
        MT_LOG_INFO(kTag, "Receive: %s", recv_buffer.data());
    }

    void CommunicateCore::send_recv_log()
    {
        MT_LOG_INFO(kTag, "Send: %s, Receive: %s", send_buffer.data(), recv_buffer.data());
    }
} // namespace APP_Communicate
