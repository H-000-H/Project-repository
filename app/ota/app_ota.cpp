/**
 * @file app_ota.cpp
 * @author H-000-H
 * @brief OTA 升级任务实现
 * @note  任务后端固定为 mini-os; 接口说明见 app_ota.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "app_ota.hpp"

#include "boot_redef.h"
#include "device.h"
#include "err.h"
#include "start.h"
#include "status.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>

namespace app
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 任务周期 (ms)
    constexpr std::uint32_t k_task_period_ms = 100;

    // 单次设备读的阻塞上限 (ms)。
    // 必须独立于任务周期: 它决定"两个字节之间允许的最大空档", 太短会在慢速链路上把一轮里
    // 已经有进展的下载判死重来。真机 115200bps 下 512B 只需 44ms, 放宽到秒级是为了容忍抖动。
    constexpr std::uint32_t k_read_timeout_ms = 3000;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 就绪握手是否已发出 (每轮 download_stream 重置一次, 见 ota_step)
    static bool s_ready_sent = false;

    Ota& Ota::get_instance()
    {
        static Ota instance;
        return instance;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    Ota::Ota()
    {
        ::device* pdev = device_find_by_label(k_dev_label);
        if (pdev == nullptr)
        {
            MT_LOG_ERROR(k_tag, "OTA uart '%s' not found", k_dev_label);
            this->m_driver = nullptr;
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(k_tag, "device_open failed");
            this->m_driver = nullptr;
            return;
        }
        MT_LOG_INFO(k_tag, "OTA uart opened");
        this->m_driver = pdev;
    }

    // 析构: 默认实现
    Ota::~Ota() = default;

    void Ota::set_fw_len(std::uint32_t len)
    {
        this->m_fw_total_len = len;
    }

    void Ota::request_ota(std::uint32_t len)
    {
        this->set_fw_len(len);
        this->start_ota();
    }

    void Ota::start_ota()
    {
        ota_open();          // 未 open 时下载会被内部拒绝 (ERR_OTA_OPEN)
        ota_rollback_open(); // 双分区+回滚: 激活后置 pending, 新固件 confirm 后清除
        this->m_is_start = true;
    }

    void Ota::stop_ota()
    {
        this->m_is_start = false;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    /**
     * @brief 传输钩子: 从 OTA 串口读取固件数据供 download_stream 消费
     * @param[in]  param   void*          设备指针 (::device*)
     * @param[out] buf     std::uint8_t*  输出缓冲, 写入读到的字节
     * @param[in]  want    std::uint32_t  期望读取的字节数
     * @param[out] out_len int*           实际读到的字节数 (成功时写入)
     * @return int ERR_OK 成功, ERR_TRANSMIT 读失败或超时
     * @note  本回调第一次被调用 == download_stream 已完成 flash 擦除、开始索要数据。此刻才通知
     *        上位机开灌: 擦除要 1~3 秒, 期间若上位机已在发会灌进 12~35KB, 远超 RX ring 容量。
     */
    static int ota_download_source(void* param, std::uint8_t* buf, std::uint32_t want, int* out_len)
    {
        ::device* dev = static_cast<::device*>(param);

        if (!s_ready_sent)
        {
            s_ready_sent = true;
            // 就绪握手走 OTA 信道本身: 上位机在同一条连接上等这个字节,
            // 走日志口等于拿诊断面当控制面, 且会与"人看日志"互斥
            static const std::uint8_t k_ready_byte = 'R';
            (void)device_write(dev, &k_ready_byte, 1u, 100u);
            MT_LOG_INFO("Ota", "OTA_READY"); // 留作人读诊断, 不当控制信号
        }

        const int got = device_read(dev, buf, want, k_read_timeout_ms);
        if (got <= 0)
        {
            return ERR_TRANSMIT;
        }
        *out_len = got;
        return ERR_OK;
    }

    // OTA 单步: 满足启动条件时流式下载 → 激活 → 复位
    void Ota::ota_step()
    {
        Ota& it = get_instance();

        if (!it.m_is_start || (it.m_driver == nullptr) || (it.m_fw_total_len == 0))
        {
            return;
        }

        MT_LOG_INFO(k_tag, "ota download started");
        s_ready_sent = false; // 每轮都会重新擦除, 所以要重新握手通知上位机
        int ret = mini_boot_source_download_stream(ota_download_source, it.m_driver, it.m_fw_total_len);
        if (ret != ERR_OK)
        {
            MT_LOG_ERROR(k_tag, "ota download failed: %d", ret);
            return;
        }

        MT_LOG_INFO(k_tag, "ota download succeeded");
        ret = mini_boot_start_ota();
        if (ret != ERR_OK)
        {
            MT_LOG_ERROR(k_tag, "ota activate failed: %d", ret);
            return;
        }

        MT_LOG_INFO(k_tag, "ota activate succeeded, system will reset");
        mini_boot_system_reset(); // 复位进 boot 运行新固件(confirm 由新固件负责)
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 任务体: 死循环执行 ota_step 并按 k_task_period_ms 让出
    void Ota::thread(void* param)
    {
        (void)param;

        for (;;)
        {
            ota_step();
            mini_os_thread_delay_ms(k_task_period_ms);
        }
    }

    bool Ota::thread_register()
    {
        mini_os_thread_t* handle = mini_os_thread_create(
            k_task_name, k_task_stack, static_cast<mini_os_uint8_t>(k_task_priority),
            [](void* param) { thread(param); }, nullptr);
        if (handle == nullptr)
        {
            MT_LOG_ERROR(k_tag, "task register failed");
            return false;
        }

        MT_LOG_INFO(k_tag, "task registered");
        return true;
    }

} // namespace app
