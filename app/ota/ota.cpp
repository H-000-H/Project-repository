#include "ota.hpp"
#include "device.h"
#include "status.h"
#include "start.h"
#include "err.h"
#include "system_log.h"

extern "C" void mini_boot_system_reset(void);/*boot_redef.h 的声明被 !__cplusplus 挡住, C++ 侧自行声明*/

namespace APP_Ota
{
    constexpr uint32_t kDelay_ms = 100; /* 任务周期/设备读写超时(ms) */

#ifndef CONFIG_XTASK_PREEMPT
    x_task Ota::s_tcb;
#endif

    Ota& Ota::get_instance()
    {
        static Ota s_instance;
        return s_instance;
    }

    Ota::Ota()
    {
        ::device* pdev = device_find_by_label(kDevLabel);
        if (pdev == nullptr)
        {
            MT_LOG_ERROR(this->kTag, "OTA uart '%s' not found", kDevLabel);
            this->ota_driver = nullptr;
            return;
        }
        if (device_open(pdev, nullptr) != MINI_OK)
        {
            MT_LOG_ERROR(this->kTag, "device_open failed");
            this->ota_driver = nullptr;
            return;
        }
        MT_LOG_INFO(this->kTag, "OTA uart opened");
        this->ota_driver = pdev;
    }

    void Ota::set_fw_len(uint32_t len)
    {
        this->fw_total_len = len;
    }

    void Ota::request_ota(uint32_t len)
    {
        set_fw_len(len);/*先定长度, 再开开关(内部 ota_open / ota_rollback_open)*/
        start_ota();
    }

    void Ota::start_ota()
    {
        ota_open();/*未open时下载会被内部拒绝(ERR_OTA_OPEN)*/
        ota_rollback_open();/*双分区+回滚: 激活后置pending, 新固件confirm后清除*/
        this->is_start = true;
    }

    void Ota::stop_ota()
    {
        this->is_start = false;
    }

    /* 传输钩子: device_read 成功返回实际字节数(可能短读), 负数为错误 */
    static int ota_download_source(void* param, uint8_t* buf, uint32_t want, int* out_len)
    {
        ::device* pdev = static_cast<::device*>(param);
        int got = device_read(pdev, buf, want, kDelay_ms);
        if (got <= 0)
        {
            return ERR_TRANSMIT;
        }
        *out_len = got;
        return ERR_OK;
    }

    void Ota::thread(x_task* self)
    {
        auto& instance = get_instance();
        PT_BEGIN(self);
        while (1)
        {
            if (instance.is_start && instance.ota_driver != nullptr && instance.fw_total_len > 0)
            {
                MT_LOG_INFO(instance.kTag, "ota download started");
                int ret = mini_boot_source_download_stream(ota_download_source, instance.ota_driver, instance.fw_total_len);
                if (ret == ERR_OK)
                {
                    MT_LOG_INFO(instance.kTag, "ota download succeeded");
                    ret = mini_boot_start_ota();
                    if (ret == ERR_OK)
                    {
                        MT_LOG_INFO(instance.kTag, "ota activate succeeded，system will reset");
                        mini_boot_system_reset();/*复位进boot运行新固件(confirm由main/新固件负责)*/
                    }
                    else
                    {
                        MT_LOG_ERROR(instance.kTag, "ota activate failed: %d", ret);
                    }
                }
                else
                {
                    MT_LOG_ERROR(instance.kTag, "ota download failed: %d", ret);
                }
            }
            PT_DELAY(self, kDelay_ms);
        }

        PT_END(self);
    }

    bool Ota::thread_register()
    {
#ifdef CONFIG_XTASK_PREEMPT
        x_task_handle_t handle = x_scheduler_task_create(
            kName, kDelay_ms, kPriority,
            [](x_task* self) { thread(self); }, nullptr);
#else
        x_task_handle_t handle = xscheduler_task_create(
            &s_tcb, kName, thread, kDelay_ms);
#endif
        if (handle == 0)
        {
            MT_LOG_ERROR(kTag, "task register failed");
            return false;
        }
        MT_LOG_INFO(kTag, "task registered");
        return true;
    }
}
