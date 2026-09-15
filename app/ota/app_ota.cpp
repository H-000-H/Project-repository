/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_ota.cpp
 * @brief OTA 升级任务实现
 * @author H-000-H
 * @note  任务后端固定为 mini-os。
 */
#include "app_ota.hpp"

#include "boot_redef.h"
#include "device.h"
#include "err.h"
#include "start.h"
#include "status.h"
#include "system_log.h"

#include "mini_backend.h"
#include "thread.h"

namespace app_ota
{

/* 任务周期 (ms) */
constexpr uint32_t kTaskPeriodMs = 100;

/* 单次设备读的阻塞上限 (ms)。
 * 必须独立于任务周期: 它决定"两个字节之间允许的最大空档", 太短会在慢速链路上
 * 把一轮里已经有进展的下载判死重来 (download_stream 一轮内累积, 任一次读超时即整轮作废)。
 * 真机 115200bps 下 512B 只需 44ms, 这里放宽到秒级纯粹是为了容忍仿真/慢链路的抖动。 */
constexpr uint32_t kReadTimeoutMs = 3000;

Ota& Ota::GetInstance()
{
    static Ota instance;
    return instance;
}

Ota::Ota()
{
    ::device* pdev = device_find_by_label(kDevLabel);
    if (pdev == nullptr)
    {
        MT_LOG_ERROR(kTag, "OTA uart '%s' not found", kDevLabel);
        driver_ = nullptr;
        return;
    }
    if (device_open(pdev, nullptr) != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "device_open failed");
        driver_ = nullptr;
        return;
    }
    MT_LOG_INFO(kTag, "OTA uart opened");
    driver_ = pdev;
}

Ota::~Ota() = default;

void Ota::SetFwLen(uint32_t len)
{
    fw_total_len_ = len;
}

void Ota::RequestOta(uint32_t len)
{
    SetFwLen(len);
    StartOta();
}

void Ota::StartOta()
{
    ota_open();          /* 未 open 时下载会被内部拒绝 (ERR_OTA_OPEN) */
    ota_rollback_open(); /* 双分区+回滚: 激活后置 pending, 新固件 confirm 后清除 */
    is_start_ = true;
}

void Ota::StopOta()
{
    is_start_ = false;
}

/* 就绪握手是否已发出 (每轮 download_stream 重置一次, 见 OtaStep) */
static bool s_ready_sent = false;

/* 传输钩子: device_read 返回实际字节数(可能短读), 负数为错误 */
static int OtaDownloadSource(void* param, uint8_t* buf, uint32_t want, int* out_len)
{
    ::device*  dev = static_cast<::device*>(param);

    /* 本回调第一次被调用 == download_stream 已完成 flash 擦除、开始索要数据。
     * 此刻才通知上位机开灌: 擦除要 1~3 秒, 期间上位机若已在发, 会灌进 12~35KB,
     * 远超 RX ring 容量; 积压损坏后整轮下载前功尽弃 (download_stream 每轮从头擦写)。 */
    if (!s_ready_sent)
    {
        s_ready_sent = true;
        /* 就绪标记走 console 日志口: OTA 口(USART3) 硬件上只接通了 RX (业务只需从 PC
         * 收固件), 无法反向发握手字节, 所以上位机改为从日志流里认这个标记。
         * 标记文本必须稳定 —— 上位机按字节匹配。 */
        MT_LOG_INFO("Ota", "OTA_READY");
    }

    const int  got = device_read(dev, buf, want, kReadTimeoutMs);
    if (got <= 0)
    {
        return ERR_TRANSMIT;
    }
    *out_len = got;
    return ERR_OK;
}

void Ota::OtaStep()
{
    Ota& it = GetInstance();

    if (!it.is_start_ || (it.driver_ == nullptr) || (it.fw_total_len_ == 0))
        return;

    MT_LOG_INFO(kTag, "ota download started");
    s_ready_sent = false; /* 每轮都会重新擦除, 所以要重新握手通知上位机 */
    int ret = mini_boot_source_download_stream(OtaDownloadSource, it.driver_, it.fw_total_len_);
    if (ret != ERR_OK)
    {
        MT_LOG_ERROR(kTag, "ota download failed: %d", ret);
        return;
    }

    MT_LOG_INFO(kTag, "ota download succeeded");
    ret = mini_boot_start_ota();
    if (ret != ERR_OK)
    {
        MT_LOG_ERROR(kTag, "ota activate failed: %d", ret);
        return;
    }

    MT_LOG_INFO(kTag, "ota activate succeeded, system will reset");
    mini_boot_system_reset(); /* 复位进 boot 运行新固件(confirm 由新固件负责) */
}

void Ota::Thread(void* param)
{
    (void)param;

    while (true)
    {
        OtaStep();
        mini_os_thread_delay_ms(kTaskPeriodMs);
    }
}

bool Ota::ThreadRegister()
{
    /* 走 mini_backend 统一任务入口 (栈大小只在这里用得上) */
    mini_task_handle_t handle = nullptr;
    const mt_err_t ret = mini_task_create_handle(
        kTaskName, app_config::kOtaTaskStack, kTaskPriority,
        [](void* param) { Thread(param); }, nullptr, -1, &handle);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "task register failed: %d", static_cast<int>(ret));
        return false;
    }

    MT_LOG_INFO(kTag, "task registered");
    return true;
}

} // namespace app_ota
