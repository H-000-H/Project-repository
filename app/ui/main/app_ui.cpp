#include "app_ui.hpp"
#include "schedule.h"
#include "system_log.h"
#include "thread.h"
#include "lvgl_port.hpp"
#include <cstdint>
namespace app
{
    constexpr const char *      kUiThreadName       = "UiThread";
    constexpr const uint32_t    kUiThreadStackSize  = 8192;
    constexpr const uint8_t     kUiThreadPriority   = 12;
    Ui& Ui::GetInstance()
    {
        static Ui s_instance;
        return s_instance;
    }

    /**
     * @brief 初始化 UI 运行时(LVGL): 幂等, 重复调用只生效一次
     * @return MINI_OK 或 MINI_ERR_*
     */
    int Ui::Init()
    {
        const int ret = display::LvglPort::get_instance().Init(); /* 开屏 + 建 display + 挂 flush 回调 */
        if (ret != MINI_OK)
        {
            MT_LOG_ERROR(kUiThreadName, "lvgl port init failed: %d", ret);
        }
        return ret;
    }

    void Ui::Thread(void* param)
    {
        (void)param;

        if (GetInstance().Init() != MINI_OK)
        {
            MT_LOG_ERROR(kUiThreadName, "ui init failed, idle");
            for (;;)
            {
                mini_os_schedule_delay(MINI_OS_MS_TO_TICK(100));
            }
        }

        for(;;)
        {
            lv_timer_handler();
            mini_os_schedule_delay(MINI_OS_MS_TO_TICK(10));
        }
    }

    bool Ui::ThreadRegister(void)
    {
        auto handle = mini_os_thread_create(kUiThreadName,kUiThreadStackSize,kUiThreadPriority,[](void* param){Thread(param);},nullptr);
        if(!handle)
        {
            MT_LOG_ERROR(kUiThreadName,"Ui thread register failed");
            return false; /* 原来这里照样往下打 success 并 return true, 上层永远发现不了注册失败 */
        }
        MT_LOG_INFO(kUiThreadName,"Ui thread register success");
        return true;
    }

} // namespace app