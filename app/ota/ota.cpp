/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file ota.cpp
 * @brief OTA 升级任务实现
 * @author H-000-H
 * @note  任务创建与循环体按后端显式分支 (裸机 xtask / mini-os), 差异不藏进宏。
 */
#include "ota.hpp"

#include "boot_redef.h"
#include "device.h"
#include "err.h"
#include "start.h"
#include "status.h"
#include "system_log.h"

#if defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
#include "mini_backend.h"
#endif
#if defined(CONFIG_OS_MINI_OS)
#include "thread.h"
#elif defined(CONFIG_OS_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
#endif

namespace app_ota
{

/* 任务周期 / 设备读写超时 (ms) */
constexpr uint32_t kTaskPeriodMs = 100;

#if defined(CONFIG_OS_BARE) && !defined(CONFIG_XTASK_PREEMPT)
x_task Ota::tcb_;
#endif

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

/* 传输钩子: device_read 返回实际字节数(可能短读), 负数为错误 */
static int OtaDownloadSource(void* param, uint8_t* buf, uint32_t want, int* out_len)
{
    ::device*  dev = static_cast<::device*>(param);
    const int  got = device_read(dev, buf, want, kTaskPeriodMs);
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

#if defined(CONFIG_OS_BARE)
void Ota::Thread(x_task* self)
{
    PT_BEGIN(self);
    while (1)
    {
        OtaStep();
        PT_DELAY(self, kTaskPeriodMs);
    }
    PT_END(self);
}
#elif defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
void Ota::Thread(void* param)
{
    (void)param;

    while (true)
    {
        OtaStep();
#if defined(CONFIG_OS_MINI_OS)
        mini_os_thread_delay_ms(kTaskPeriodMs);
#else
        vTaskDelay(pdMS_TO_TICKS(kTaskPeriodMs));
#endif
    }
}
#else
#error "ota.cpp 尚未适配该 OS 后端: 请补上 Thread 循环与该内核的毫秒延时"
#endif

bool Ota::ThreadRegister()
{
#if defined(CONFIG_OS_BARE) && defined(CONFIG_XTASK_PREEMPT)
    /* 抢占式: TCB 由任务池分配, 需要优先级 */
    x_task_handle_t handle = x_scheduler_task_create(
        kTaskName, kTaskPeriodMs, kTaskPriority, [](x_task* self) { Thread(self); }, nullptr);
    if (handle == 0)
    {
        MT_LOG_ERROR(kTag, "task register failed");
        return false;
    }
#elif defined(CONFIG_OS_BARE)
    /* 协调式: TCB 由本类静态提供, 无优先级概念 */
    x_task_handle_t handle = xscheduler_task_create(&tcb_, kTaskName, Thread, kTaskPeriodMs);
    if (handle == 0)
    {
        MT_LOG_ERROR(kTag, "task register failed");
        return false;
    }
#elif defined(CONFIG_OS_MINI_OS) || defined(CONFIG_OS_FREERTOS)
    /* 真线程后端: 走 mini_backend 统一任务入口 (栈大小只在这里用得上) */
    mini_task_handle_t handle = nullptr;
    const mt_err_t ret = mini_task_create_handle(
        kTaskName, app_config::kOtaTaskStack, kTaskPriority,
        [](void* param) { Thread(param); }, nullptr, -1, &handle);
    if (ret != MINI_OK)
    {
        MT_LOG_ERROR(kTag, "task register failed: %d", static_cast<int>(ret));
        return false;
    }
#else
#error "ota.cpp 尚未适配该 OS 后端: 请补上任务创建分支"
#endif

    MT_LOG_INFO(kTag, "task registered");
    return true;
}

} // namespace app_ota
