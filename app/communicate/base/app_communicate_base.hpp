/**
 * @file app_communicate_base.hpp
 * @author H-000-H
 * @brief 通信模块: 通用读写底座 + CRTP 单例/任务基类
 * @note  CommunicateCore 只依赖 device 的 read/write, 不绑总线类型, 也不含单例/任务;
 *        Communicate<Derived> 给每个派生类生成一份单例 + 一条任务。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_COMMUNICATE_BASE_HPP
#define APP_COMMUNICATE_BASE_HPP
#include <cstddef>
#include <cstdint>

#include "device.h"
#include "etl/array.h"
#include "etl/optional.h"
#include "etl/span.h"
#include "status.h"
#include "system_log.h"

#include "thread.h"

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 通用通信底座 (无单例/无任务); 发送长度取 span.size(), 上限 k_buffer_size
    class CommunicateCore
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 收发缓冲上限 (字节)
        static constexpr std::size_t k_buffer_size = 128;

        // 收到数据时的业务回调; data 指向内部接收缓冲, len 为实际字节数
        using RxCallback = void (*)(const uint8_t* data, size_t len);

    protected:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 构造: 按 label 查找并打开 device, 失败则记录日志并保持未绑定
         * @param[in] label const char* DTS 节点 label
         */
        explicit CommunicateCore(const char* label);

        CommunicateCore(const CommunicateCore&)            = delete;
        CommunicateCore& operator=(const CommunicateCore&) = delete;
        CommunicateCore(CommunicateCore&&)                 = delete;
        CommunicateCore& operator=(CommunicateCore&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        ::device*                          m_dev = nullptr;
        etl::array<uint8_t, k_buffer_size> m_send_buffer{};
        etl::array<uint8_t, k_buffer_size> m_recv_buffer{};
        std::size_t                        m_rx_len      = 0;       // 最近一次读到的有效字节数
        RxCallback                         m_rx_callback = nullptr; // 为空时只打日志

    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        virtual ~CommunicateCore();

        /**
         * @brief 注册接收回调 (传 nullptr 恢复为只打日志)
         * @param[in] callback RxCallback 收到数据时调用的业务回调
         */
        void set_rx_callback(RxCallback callback);

        /**
         * @brief 发送
         * @param[in] data_view etl::span<const uint8_t> 待发送数据视图
         * @param[in] time_out  uint32_t 发送超时 (ms), 0 表示用 k_default_timeout
         * @return etl::optional<mt_err_t> 有值时为 device_write 的错误码
         */
        etl::optional<mt_err_t> send(etl::span<const uint8_t> data_view, uint32_t time_out);

        /**
         * @brief 主动读一次(不触发回调), 数据进接收缓冲
         * @param[in] time_out uint32_t 读超时 (ms), 0 表示用 k_default_timeout
         * @return etl::optional<mt_err_t> 读结果错误码 (MINI_OK / MINI_ERR_TIMEOUT / 其他)
         */
        etl::optional<mt_err_t> receive(uint32_t time_out);

        // 最近一次 receive()/poll_once() 读到的有效字节数
        std::size_t get_rx_len() const;

        // 最近一次读到的数据视图 (指向接收缓冲, 长度为 get_rx_len())
        etl::span<const uint8_t> get_rx() const;

        /**
         * @brief 轮询一次: 读 → 分发(有回调给回调, 否则打印)
         * @param[in] time_out uint32_t 轮询读超时 (ms)
         * @return int >0 收到的字节数 / 0 无数据 / <0 错误
         */
        int poll_once(uint32_t time_out);

        /**
         * @brief 半双工一写一读(先发后收): 底座默认不支持, UART 派生类 override
         * @param[in] data_view etl::span<const uint8_t> 待发送数据视图
         * @param[in] time_out  uint32_t 传输超时 (ms), 0 表示用 k_default_timeout
         * @return etl::optional<mt_err_t> 底座恒返回 MINI_ERR_NOTSUPP
         */
        virtual etl::optional<mt_err_t> send_resv(etl::span<const uint8_t> data_view, uint32_t time_out);

        // 打印发送缓冲 (字符串形式)
        void send_log();

        // 打印接收缓冲 (字符串形式)
        void recv_log();

        // 发送与接收缓冲一起打印
        void send_recv_log();

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        static constexpr const char*   k_tag             = "communicate";
        static constexpr std::uint16_t k_default_timeout = 1000; // 发送/主动读超时 (ms)
        /* 轮询读超时必须短: 无数据时若用 k_default_timeout 会把任务阻塞 1 秒, 拖慢同优先级任务 */
        static constexpr std::uint32_t k_poll_timeout = 2;
    };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    /**
     * @brief 通信基类(CRTP): 给每个派生类生成一份单例 + 任务注册样板
     * @tparam Derived 派生类(如 UartCommunicate), 必须提供 thread() / k_thread_period_ms / k_task_stack
     * @note  周期归派生类定义, 它同时是 thread 里让出的取值 —— 一处定义, 避免两个节奏打架
     */
    template <typename Derived>
    class Communicate : public CommunicateCore
    {
    protected:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 构造: 转发 label 给 CommunicateCore
         * @param[in] label const char* DTS 节点 label
         */
        explicit Communicate(const char* label) : CommunicateCore(label) {}

    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 本派生类的唯一实例 (首次调用时构造)
        static Derived& get_instance()
        {
            static Derived instance;
            return instance;
        }

        /**
         * @brief 注册本派生类的轮询任务
         * @return bool 创建成功返回 true, 失败返回 false
         */
        bool thread_register()
        {
            mini_os_thread_t* handle = mini_os_thread_create(
                CommunicateCore::k_tag, Derived::k_task_stack, static_cast<mini_os_uint8_t>(Derived::k_task_priority),
                &Derived::thread, nullptr);
            if (handle == nullptr)
            {
                MT_LOG_ERROR(CommunicateCore::k_tag, "task register failed");
                return false;
            }
            MT_LOG_INFO(CommunicateCore::k_tag, "task registered");
            return true;
        }
    };
} // namespace app

#endif // APP_COMMUNICATE_BASE_HPP
