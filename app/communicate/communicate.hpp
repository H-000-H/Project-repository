/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file communicate.hpp
 * @brief Communication module header file.
 * @author H-000-H
 * @details 两层结构:
 *          - CommunicateCore  通用通信实现: 只依赖 device 的 read/write, 不绑总线类型,
 *                             也不含单例/任务(它是实现底座, 不是给业务用的入口)
 *          - Communicate<Derived>  CRTP 基类: 给**每个派生类**自动生成一份真单例
 *                             (static Derived) + 一条协程任务(static thread/s_tcb)
 *          所以换接收源(串口/WiFi/CAN…)只需新增一个派生类, 各自单例互不干扰,
 *          也可以多路并存各跑各的任务; UART 专有的 send_resv 由 UartCommunicate override。
 *
 *          收发长度说明:
 *          - 发送长度 = span.size(), 上限 kBufferSize(不再是固定 128)
 *          - 接收长度 = get_rx_len() (device_read 返回的就是实际字节数)
 */
#ifndef COMMUNICATE_HPP
#define COMMUNICATE_HPP
#include <cstdint>
#include <cstddef>
#include "device.h"
#include "etl/optional.h"
#include "status.h"
#include "etl/span.h"
#include "etl/array.h"
#include "xtask.h"
#include "system_log.h"
namespace APP_Communicate 
{
    constexpr static std::size_t kBufferSize = 128;

    /* ---------------------------------------------------------------------- */
    /* 通用通信实现(底座, 无单例/无任务)                                        */
    /* ---------------------------------------------------------------------- */
    class CommunicateCore
    {
    public:
        /** @brief 收到数据时的业务回调: data 指向内部 recv_buffer, len 为实际字节数 */
        using rx_handler_t = void (*)(const uint8_t* data, size_t len);

    protected:
        explicit CommunicateCore(const char* label);
        CommunicateCore(const CommunicateCore&) = delete;
        CommunicateCore& operator=(const CommunicateCore&) = delete;
        CommunicateCore(CommunicateCore&&) = delete;
        CommunicateCore& operator=(CommunicateCore&&) = delete;

        ::device* pdev = nullptr;
        etl::array<uint8_t, kBufferSize> send_buffer{}; 
        etl::array<uint8_t, kBufferSize> recv_buffer{};
        std::size_t rx_len = 0;             /* 最近一次读到的有效字节数 */
        rx_handler_t rx_handler = nullptr;  
    public:
        virtual ~CommunicateCore() = default;

        /** @brief 注册接收回调(thread 收到数据时回调; 传 nullptr 恢复为只打日志) */
        void set_rx_handler(rx_handler_t fn) { rx_handler = fn; }

        /**
         * @brief 发送: 长度取 data_view.size(), 上限 kBufferSize
         * @param[in] data_view 待发数据(动态长度 span, 不再要求凑满 128)
         * @param[in] time_out 超时(ms), 0 用 kDefaultTimeout
         */
        etl::optional<mt_err_t> send(etl::span<const uint8_t> data_view, uint32_t time_out);

        /**
         * @brief 主动读一次(不触发回调), 数据进 recv_buffer
         * @return MINI_OK 读到数据(长度见 get_rx_len()); MINI_ERR_TIMEOUT 无数据;
         *         MINI_ERR_NODEV 设备未就绪; 其他负数为错误
         */
        etl::optional<mt_err_t> receive(uint32_t time_out);

        /** @brief 最近一次 receive()/poll_once() 读到的有效字节数 */
        std::size_t get_rx_len() const { return rx_len; }

        /** @brief 最近一次读到的数据长度  */
        etl::span<const uint8_t> get_rx() const
        {
            return etl::span<const uint8_t>(recv_buffer.data(), rx_len);
        }

        /**
         * @brief 协程轮询一次: 读 -> 分发(有回调给回调, 否则打印)
         * @return >0 读到的字节数; 0 无数据(超时, 正常); <0 错误
         * @note  零字节超时不算错误, 不会打日志, 免得轮询刷屏
         */
        int poll_once(uint32_t time_out);

        /** @brief 半双工一写一读(先发后收): 底座默认不支持, UART 派生类 override */
        virtual etl::optional<mt_err_t> send_resv(etl::span<const uint8_t> data_view, uint32_t time_out);

        void send_log();
        void recv_log();
        void send_recv_log();

        static constexpr const char* kTag = "communicate";
        static constexpr std::uint16_t kDefaultTimeout = 1000; // 发送/主动读超时 (ms)
        /** 轮询读超时 (ms): 必须短。裸机协调式调度下 poll_once 是同步忙等,
         *  无数据时若用 kDefaultTimeout 会把主循环(含其他任务/喂狗)钉住整整 1 秒 */
        static constexpr std::uint32_t kPollTimeout = 2;
        static constexpr std::size_t kThtreadDelay = 100; // Default thread delay in milliseconds
    };

    /* ---------------------------------------------------------------------- */
    /* CRTP: 每个派生类一份单例 + 一条任务                                      */
    /* ---------------------------------------------------------------------- */
    /**
     * @brief 通信基类(CRTP) — 提供"每派生类唯一"的单例与协程任务
     * @tparam Derived 派生类(如 UartCommunicate), 必须继承 Communicate<Derived>
     * @note  单例是 static Derived(不是基类指针),而且是模板更不可能互相干扰
     */
    template <typename Derived>
    class Communicate : public CommunicateCore
    {
    protected:
        explicit Communicate(const char* label) : CommunicateCore(label) {}
    public:
        /** @brief 本派生类的唯一实例(首次调用时构造) */
        static Derived& getInstance()
        {
            static Derived s_instance;
            return s_instance;
        }
        static void thread(x_task* self)
        {
            Derived& it = getInstance();

            PT_BEGIN(self);
            while(true)
            {
                it.poll_once(CommunicateCore::kPollTimeout);
                PT_DELAY(self, CommunicateCore::kThtreadDelay);
            }
            PT_END(self);
        }

        /** @brief 把本派生类的任务注册到调度器(每派生类一个 TCB) */
        bool thread_register(void)
        {
            static x_task s_tcb;   /* 函数内静态: 每实例化一份*/

            x_task_handle_t handle = xscheduler_task_create(&s_tcb, CommunicateCore::kTag, thread,
                                                            CommunicateCore::kThtreadDelay);
            if (handle == 0)
            {
                MT_LOG_ERROR(CommunicateCore::kTag, "task register failed");
                return false;
            }
            MT_LOG_INFO(CommunicateCore::kTag, "task registered");
            return true;
        }
    };
} // namespace APP_Communicate
#endif // COMMUNICATE_HPP
