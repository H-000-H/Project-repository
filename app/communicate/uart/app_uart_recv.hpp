/**
 * @file app_uart_recv.hpp
 * @author H-000-H
 * @brief UART 通信派生类声明
 * @details 在通用读写(send/receive)之上提供 UART 专有的半双工 send_resv
 *          (uart_transfer_arg + UART_CMD_TRANSFER ioctl)。
 *          全仓只有 app_uart_recv.cpp include vfs-uart.h, 底座不碰 UART 专有接口。
 *          get_instance() / thread_register() 由 CRTP 基类生成; thread() 必须由本类自己实现。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_UART_RECV_HPP
#define APP_UART_RECV_HPP
#include <cstdint>

#include "app_communicate_base.hpp"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    class UartCommunicate final : public Communicate<UartCommunicate>
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /* public 是为了让 CRTP 基类的 get_instance() 能构造本类实例; 使用走 get_instance() */

        // 构造: 绑定 k_device_name 对应的通信设备
        UartCommunicate();

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 本类绑定的设备标签 (DTS 里 communicate client 节点的 label)
        static constexpr const char* k_device_name = "communicate";

        // 任务周期 (ms): 任务注册与 thread 里让出共用这个值
        static constexpr unsigned int k_thread_period_ms = 100;

        // mini-os: 数值越小越优先 (与 LED 同级)
        static constexpr unsigned int  k_task_priority = 12;
        static constexpr std::uint32_t k_task_stack    = 1024;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 轮询任务体: 死循环 poll_once 并按 k_thread_period_ms 让出
         * @param[in] param void* 线程参数 (未使用)
         */
        static void thread(void* param);

        /**
         * @brief 半双工一写一读: 走 UART_CMD_TRANSFER (先发 tx 再收 rx)
         * @param[in] data_view etl::span<const uint8_t> 待发送数据视图 (长度取 size(), 上限 k_buffer_size)
         * @param[in] time_out  uint32_t 传输超时 (ms), 0 表示用 k_default_timeout
         * @return etl::optional<mt_err_t> 传输结果错误码 (MINI_OK / MINI_ERR_TIMEOUT / 其他)
         */
        etl::optional<mt_err_t> send_resv(etl::span<const uint8_t> data_view, uint32_t time_out) override;
    };
} // namespace app

#endif // APP_UART_RECV_HPP
