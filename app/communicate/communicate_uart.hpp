/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file communicate_uart.hpp
 * @brief UART 通信派生类声明.
 * @author H-000-H
 * @details 在通用读写(send/receive)之上提供 UART 专有的半双工 send_resv
 *          (uart_transfer_arg + UART_CMD_TRANSFER ioctl)。
 *          全仓只有 communicate_uart.cpp include vfs-uart.h, 底座不碰 UART 专有接口。
 *          getInstance() / thread() / thread_register() 都由 CRTP 基类 Communicate<> 生成,
 *          本类不用再写单例样板。
 */
#ifndef COMMUNICATE_UART_HPP
#define COMMUNICATE_UART_HPP
#include "communicate.hpp"
namespace APP_Communicate 
{
    class UartCommunicate final : public Communicate<UartCommunicate>
    {
    public:
        /* public 是为了让 CRTP 基类的 getInstance() 能构造本类实例;
         * 实际使用走 getInstance()*/
        UartCommunicate();

        /** @brief 本类绑定的设备标签(DTS 里 &communicate 节点的 label) */
        static constexpr const char* kDeviceName = "communicate";

        /** @brief 半双工一写一读: 走 UART_CMD_TRANSFER(先发 tx 再收 rx), 发送长度取 span.size() */
        etl::optional<mt_err_t> send_resv(etl::span<const uint8_t> data_view, uint32_t time_out) override;
    };
} // namespace APP_Communicate
#endif // COMMUNICATE_UART_HPP
