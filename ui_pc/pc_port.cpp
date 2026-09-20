/*
 * PC 侧适配桩: 只把 UI 层用到的嵌入接口补上, 让它在 PC 上能编译并跑起来。
 * 真机上这些由 board/hal/mini-os 提供, 这里是最小实现。
 */
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "device.h"
#include "display_drv.h"
#include "mini_time.h"
#include "pc_display.h"
#include "schedule.h"
#include "thread.h"
#include "vfs-gpio.h"

extern "C"
{

    /* ---- 设备层: 一个假设备, 让 Open/Flush 能跑通 ---- */
    static struct device s_fake_dev;

    struct device* device_find_by_label(const char* label)
    {
        (void)label;
        return &s_fake_dev;
    }

    mt_err_t device_open(struct device* pdev, void* arg)
    {
        (void)pdev;
        (void)arg;
        return MINI_OK;
    }

    mt_err_t device_close(struct device* pdev)
    {
        (void)pdev;
        return MINI_OK;
    }

    /* 面板走 PC 窗口: GET_INFO 报尺寸/格式, FLUSH 把像素贴到窗口 */
    mt_err_t device_ioctl(struct device* pdev, int cmd, void* arg, size_t arg_len, uint32_t timeout_ms)
    {
        (void)pdev;
        (void)timeout_ms;

        switch (cmd)
        {
        case DISPLAY_CMD_GET_INFO:
            if (arg != nullptr)
            {
                struct display_info_arg* info = static_cast<struct display_info_arg*>(arg);
                info->width  = static_cast<uint16_t>(kPcPanelWidth);
                info->height = static_cast<uint16_t>(kPcPanelHeight);
                info->format = DISPLAY_FMT_RGB565;
            }
            return MINI_OK;

        case DISPLAY_CMD_FLUSH:
            if (arg != nullptr)
            {
                const struct display_draw_arg* a = static_cast<const struct display_draw_arg*>(arg);
                if ((a->format == DISPLAY_FMT_RGB565) && (a->data != nullptr))
                {
                    pc_display_blit(a->x, a->y, a->w, a->h, reinterpret_cast<const uint16_t*>(a->data), true);
                }
            }
            return MINI_OK;

        case GPIO_CMD_GET_LEVEL:
            /* PC 上没接按键: 电平全 0 (未按下) */
            if (arg != nullptr)
            {
                std::memset(arg, 0, arg_len);
            }
            return MINI_OK;

        default:
            return MINI_OK;
        }
    }

    /* ---- 时间与调度 ---- */
    uint32_t mini_time_ms(void)
    {
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    void mini_os_schedule_delay(mini_os_uint32_t ticks)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
    }

    /* mini_tree/core/src/mini_time.c 没进 PC 构建(ui_logic 只编 app/ui + ui + 本桩文件),
     * app/ui 里用到的 mini_delay_ms 在这儿补个等价实现; 真机上这个是 mini_tree 提供的 */
    void mini_delay_ms(uint32_t ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    mini_os_thread_t* mini_os_thread_create(const char* name, mini_os_uint32_t stack_size, mini_os_uint8_t priority,
                                            void (*entry)(void*), void* param)
    {
        (void)name;
        (void)stack_size;
        (void)priority;
        std::thread(entry, param).detach();
        static int s_dummy_handle;
        return reinterpret_cast<mini_os_thread_t*>(&s_dummy_handle);
    }

    /* ---- 日志: 直接打到 stdout ---- */
    int mini_log_get_tick(void)
    {
        return static_cast<int>(mini_time_ms());
    }

    void mini_log_default_output(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stdout, fmt, ap);
        va_end(ap);
        std::fflush(stdout);
    }
}
