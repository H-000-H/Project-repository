/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_uart_recv.cpp
 * @brief UART 通信派生类实现
 * @author H-000-H
 * @note  仅本文件依赖 UART 专有的 uart_transfer_arg / UART_CMD_TRANSFER。
 *        GetInstance() 由 CRTP 基类 Communicate<UartCommunicate> 生成, 此处无需定义。
 * @note  本层不感知业务: 收到数据的处理由业务侧 SetRxHandler() 注入。
 */
#include "app_uart_recv.hpp"

#include <etl/algorithm.h>
#include <etl/optional.h>

#include "status.h"
#include "system_log.h"
#include "vfs-uart.h"

#include "mini_backend.h"
#include "thread.h"

namespace app_communicate
{

UartCommunicate::UartCommunicate() : Communicate(kDeviceName){}

void UartCommunicate::Thread(void* param)
{
    (void)param;

    UartCommunicate& it = GetInstance();
    while (true)
    {
        it.PollOnce(kPollTimeout);
        mini_os_thread_delay_ms(kThreadPeriodMs);
    }
}

etl::optional<mt_err_t> UartCommunicate::SendResv(etl::span<const uint8_t> data_view,
                                                  uint32_t time_out)
{
    const std::size_t tx_len = data_view.size();
    if ((tx_len == 0) || (tx_len > kBufferSize))
    {
        MT_LOG_ERROR(kTag, "invalid send_resv length: %u (max %u)", (unsigned)tx_len,
                     (unsigned)kBufferSize);
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_INVAL));
    }
    if (dev_ == nullptr) /* 构造时已打过 ERROR */
    {
        rx_len_ = 0u;
        MT_LOG_ERROR(kTag, "device not ready, send_resv aborted");
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_NODEV));
    }
    if (time_out == 0)
    {
        time_out = kDefaultTimeout;
    }

    ::uart_transfer_arg transfer_arg{};
    etl::copy(data_view.begin(), data_view.end(), send_buffer_.begin());
    transfer_arg.tx     = send_buffer_.data();
    transfer_arg.rx     = recv_buffer_.data();
    transfer_arg.tx_len = tx_len;
    transfer_arg.rx_len = recv_buffer_.size();

    const int n = device_ioctl(dev_, UART_CMD_TRANSFER, &transfer_arg,
                               sizeof(::uart_transfer_arg), time_out);

    rx_len_ = (n > 0) ? static_cast<std::size_t>(n) : 0u;
    if (n > 0)
    {
        return etl::make_optional(static_cast<mt_err_t>(MINI_OK));
    }
    if (n == MINI_ERR_TIMEOUT) /* 对端没回 */
    {
        return etl::make_optional(static_cast<mt_err_t>(MINI_ERR_TIMEOUT));
    }
    MT_LOG_ERROR(kTag, "device_ioctl(UART_CMD_TRANSFER) failed: %d", n);
    return etl::make_optional(static_cast<mt_err_t>(n));
}

} // namespace app_communicate
