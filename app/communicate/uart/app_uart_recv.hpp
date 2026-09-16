/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_uart_recv.hpp
 * @brief UART 通信派生类声明
 * @author H-000-H
 * @details 在通用读写(Send/Receive)之上提供 UART 专有的半双工 SendResv
 *          (uart_transfer_arg + UART_CMD_TRANSFER ioctl)。
 *          全仓只有 app_uart_recv.cpp include vfs-uart.h, 底座不碰 UART 专有接口。
 *          GetInstance() / ThreadRegister() 由 CRTP 基类生成, 本类不用写单例样板;
 *          Thread() 必须由本类自己实现(各设备的轮询节奏不同, 基类不预设)。
 */
#ifndef APP_COMMUNICATE_UART_APP_UART_RECV_HPP_
#define APP_COMMUNICATE_UART_APP_UART_RECV_HPP_

#include <cstdint>

#include "app_communicate_base.hpp"

namespace app
{

class UartCommunicate final : public Communicate<UartCommunicate>
{
public:
    /* public 是为了让 CRTP 基类的 GetInstance() 能构造本类实例; 使用走 GetInstance() */
    UartCommunicate();

    /** @brief 本类绑定的设备标签 (DTS 里 communicate client 节点的 label) */
    static constexpr const char* kDeviceName = "communicate";

    /* 任务周期 (ms): 任务注册与 Thread 里让出共用这个值 */
    static constexpr unsigned int kThreadPeriodMs = 100;

    /* mini-os: 数值越小越优先 (与 LED 同级) */
    static constexpr unsigned int  kTaskPriority = 12;
    static constexpr std::uint32_t kTaskStack    = 1024;

    static void Thread(void* param);

    /** @brief 半双工一写一读: 走 UART_CMD_TRANSFER(先发 tx 再收 rx), 长度取 span.size() */
    etl::optional<mt_err_t> SendResv(etl::span<const uint8_t> data_view, uint32_t time_out) override;
};

} // namespace app

#endif // APP_COMMUNICATE_UART_APP_UART_RECV_HPP_
