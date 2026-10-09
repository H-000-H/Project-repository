/**
 * @file app_uart_recv.cpp
 * @author H-000-H
 * @brief UART 通信派生类实现
 * @note  仅本文件依赖 UART 专有的 uart_transfer_arg / UART_CMD_TRANSFER;
 *        本层不感知业务, 收到数据的处理由业务侧 set_rx_callback() 注入。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "app_uart_recv.hpp"

#include <etl/algorithm.h>
#include <etl/optional.h>

#include "status.h"
#include "system_log.h"
#include "thread.h"
#include "vfs-uart.h"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    UartCommunicate::UartCommunicate() : Communicate(k_device_name)
    {
    }

    // 轮询任务体: 死循环 poll_once 并按 k_thread_period_ms 让出
    void UartCommunicate::thread(void* param)
    {
        (void)param;

        UartCommunicate& it = get_instance();
        for (;;)
        {
            it.poll_once(k_poll_timeout);
            mini_os_thread_delay_ms(k_thread_period_ms);
        }
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 半双工一写一读: 走 UART_CMD_TRANSFER (先发 tx 再收 rx)
    etl::optional<mt_err_t> UartCommunicate::send_resv(etl::span<const std::uint8_t> data_view, std::uint32_t time_out)
    {
        const std::size_t tx_len = data_view.size();
        if ((tx_len == 0) || (tx_len > k_buffer_size))
        {
            MT_LOG_ERROR(k_tag, "invalid send_resv length: %u (max %u)",
                         static_cast<unsigned>(tx_len), static_cast<unsigned>(k_buffer_size));
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
        }
        if (this->m_dev == nullptr) // 构造时已打过 ERROR
        {
            this->m_rx_len = 0u;
            MT_LOG_ERROR(k_tag, "device not ready, send_resv aborted");
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
        }
        if (time_out == 0)
        {
            time_out = k_default_timeout;
        }

        ::uart_transfer_arg transfer_arg{};
        etl::copy(data_view.begin(), data_view.end(), this->m_send_buffer.begin());
        transfer_arg.tx     = this->m_send_buffer.data();
        transfer_arg.rx     = this->m_recv_buffer.data();
        transfer_arg.tx_len = tx_len;
        transfer_arg.rx_len = this->m_recv_buffer.size();

        const int n = device_ioctl(this->m_dev, UART_CMD_TRANSFER, &transfer_arg,
                                   sizeof(::uart_transfer_arg), time_out);

        this->m_rx_len = (n > 0) ? static_cast<std::size_t>(n) : 0u;
        if (n > 0)
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
        }
        if (n == MINI_ERR_TIMEOUT) // 对端没回
        {
            return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));
        }
        MT_LOG_ERROR(k_tag, "device_ioctl(UART_CMD_TRANSFER) failed: %d", n);
        return etl::make_optional(static_cast<mt_err_t>(n));
    }

} // namespace app
