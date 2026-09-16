/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_communicate_base.hpp
 * @brief 通信模块: 通用读写底座 + CRTP 单例/任务基类
 * @author H-000-H
 * @details 两层结构:
 *          - CommunicateCore  通用通信实现: 只依赖 device 的 read/write, 不绑总线类型,
 *                             也不含单例/任务(它是实现底座, 不是给业务用的入口)
 *          - Communicate<Derived>  CRTP 基类: 给**每个派生类**自动生成一份真单例
 *                             (static Derived) + 一条任务
 *          所以换接收源(串口/WiFi/CAN…)只需新增一个派生类, 各自单例互不干扰,
 *          也可以多路并存各跑各的任务; UART 专有的 SendResv 由 UartCommunicate override。
 *
 *          收发长度说明:
 *          - 发送长度 = span.size(), 上限 kBufferSize
 *          - 接收长度 = GetRxLen() (device_read 返回的就是实际字节数)
 */
#ifndef APP_COMMUNICATE_BASE_APP_COMMUNICATE_BASE_HPP_
#define APP_COMMUNICATE_BASE_APP_COMMUNICATE_BASE_HPP_

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

/* -------------------------------------------------------------------------- */
/* 通用通信实现 (底座, 无单例/无任务)                                          */
/* -------------------------------------------------------------------------- */
class CommunicateCore
{
public:
    /** @brief 收发缓冲上限 (字节) */
    static constexpr std::size_t kBufferSize = 128;

    /** @brief 收到数据时的业务回调: data 指向内部接收缓冲, len 为实际字节数 */
    using RxCallback = void (*)(const uint8_t* data, size_t len);

protected:
    explicit CommunicateCore(const char* label);
    CommunicateCore(const CommunicateCore&) = delete;
    CommunicateCore& operator=(const CommunicateCore&) = delete;
    CommunicateCore(CommunicateCore&&) = delete;
    CommunicateCore& operator=(CommunicateCore&&) = delete;

    ::device* m_dev = nullptr;
    etl::array<uint8_t, kBufferSize> m_send_buffer{};
    etl::array<uint8_t, kBufferSize> m_recv_buffer{};
    std::size_t m_rx_len      = 0;        /**< 最近一次读到的有效字节数 */
    RxCallback  m_rx_callback = nullptr;  /**< 业务回调, 为空时只打日志 */

public:
    virtual ~CommunicateCore();

    /** @brief 注册接收回调 (传 nullptr 恢复为只打日志) */
    void SetRxCallback(RxCallback callback);

    /** @brief 发送: 长度取 data_view.size(), 上限 kBufferSize */
    etl::optional<mt_err_t> Send(etl::span<const uint8_t> data_view, uint32_t time_out);

    /** @brief 主动读一次(不触发回调), 数据进接收缓冲 */
    etl::optional<mt_err_t> Receive(uint32_t time_out);

    /** @brief 最近一次 Receive()/PollOnce() 读到的有效字节数 */
    std::size_t GetRxLen() const;

    /** @brief 最近一次读到的数据视图 */
    etl::span<const uint8_t> GetRx() const;

    /** @brief 轮询一次: 读 → 分发(有回调给回调, 否则打印); 返回 >0 字节数 / 0 无数据 / <0 错误 */
    int PollOnce(uint32_t time_out);

    /** @brief 半双工一写一读(先发后收): 底座默认不支持, UART 派生类 override */
    virtual etl::optional<mt_err_t> SendResv(etl::span<const uint8_t> data_view,
                                             uint32_t time_out);

    void SendLog();
    void RecvLog();
    void SendRecvLog();

    static constexpr const char*   kTag            = "communicate";
    static constexpr std::uint16_t kDefaultTimeout = 1000; /**< 发送/主动读超时 (ms) */

    /* 轮询读超时 (ms) 必须短: 无数据时若用 kDefaultTimeout 会把任务整整阻塞 1 秒,
     * 拖慢同优先级任务的心跳 */
    static constexpr std::uint32_t kPollTimeout = 2;
};

/* -------------------------------------------------------------------------- */
/* CRTP: 每个派生类一份单例 + 一条任务                                         */
/* -------------------------------------------------------------------------- */
/**
 * @brief 通信基类(CRTP) — 提供"每派生类唯一"的单例 + 任务注册样板
 * @tparam Derived 派生类(如 UartCommunicate), 必须继承 Communicate<Derived>
 * @note  单例是 static Derived(不是基类指针), 而且是模板更不可能互相干扰
 * @note  派生类必须提供三样东西(漏写会在编译期报错, 不会静默退化):
 *          - `static void Thread(void*)`                      任务循环体
 *          - `static constexpr unsigned int kThreadPeriodMs`  任务周期 (ms)
 *          - `static constexpr std::uint32_t kTaskStack`      任务栈大小
 *        周期归派生类定义, 它同时是 Thread 里让出的取值 —— 一处定义,
 *        避免"调度唤醒"与"休眠让出"两个节奏打架。
 */
template <typename Derived>
class Communicate : public CommunicateCore
{
protected:
    explicit Communicate(const char* label) : CommunicateCore(label) {}

public:
    /** @brief 本派生类的唯一实例(首次调用时构造) */
    static Derived& GetInstance()
    {
        static Derived instance;
        return instance;
    }

    /** @brief 通过模板特性顶层虽然都是这个register但是确实不同的 */
    bool ThreadRegister()
    {
        mini_os_thread_t* handle = mini_os_thread_create(
            CommunicateCore::kTag, Derived::kTaskStack, static_cast<mini_os_uint8_t>(Derived::kTaskPriority),
            &Derived::Thread, nullptr);
        if (handle == nullptr)
        {
            MT_LOG_ERROR(CommunicateCore::kTag, "task register failed");
            return false;
        }
        MT_LOG_INFO(CommunicateCore::kTag, "task registered");
        return true;
    }
};

} // namespace app

#endif // APP_COMMUNICATE_BASE_APP_COMMUNICATE_BASE_HPP_
