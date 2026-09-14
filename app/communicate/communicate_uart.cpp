/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file communicate_uart.cpp
 * @brief UART 通信派生类实现.
 * @author H-000-H
 * @note  仅本文件依赖 UART 专有的 uart_transfer_arg / UART_CMD_TRANSFER。
 *        getInstance() 由 CRTP 基类 Communicate<UartCommunicate> 生成, 此处无需定义。
 * @note  本层不感知业务: 收到数据的处理由业务侧 set_rx_handler() 注入。
 */
#include "communicate_uart.hpp"
#include "vfs-uart.h"
#include "status.h"
#include "system_log.h"
#include <etl/optional.h>
#include <etl/algorithm.h>

namespace APP_Communicate 
{
    UartCommunicate::UartCommunicate() : Communicate(kDeviceName)
    {
    }

    etl::optional<mt_err_t> UartCommunicate::send_resv(etl::span<const uint8_t> data_view, uint32_t time_out)
    {
        const std::size_t tx_len = data_view.size();
        if ((tx_len == 0) || (tx_len > kBufferSize))
        {
            MT_LOG_ERROR(kTag, "invalid send_resv length: %u (max %u)", (unsigned)tx_len, (unsigned)kBufferSize);
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
        }
        /* 设备未就绪: 构造时已打过 ERROR */
        if (this->pdev == nullptr)
        {
            rx_len = 0u;
            MT_LOG_ERROR(kTag, "device not ready, send_resv aborted");
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = CommunicateCore::kDefaultTimeout;
        }
        ::uart_transfer_arg m_transfer_arg{};
        etl::copy(data_view.begin(), data_view.end(), send_buffer.begin());
        m_transfer_arg.tx = send_buffer.data();
        m_transfer_arg.rx = recv_buffer.data();
        m_transfer_arg.tx_len = tx_len;                 
        m_transfer_arg.rx_len = recv_buffer.size();
        const int n = device_ioctl(this->pdev, UART_CMD_TRANSFER, &m_transfer_arg, sizeof(::uart_transfer_arg), time_out);

        rx_len = (n > 0) ? static_cast<std::size_t>(n) : 0u;
        if (n > 0)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
        }
        if (n == MINI_ERR_TIMEOUT)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));   /* 对端没回 */
        }
        MT_LOG_ERROR(kTag, "device_ioctl(UART_CMD_TRANSFER) failed: %d", n);
        return etl::make_optional(static_cast<mt_err_t>(n));
    }

} // namespace APP_Communicate
