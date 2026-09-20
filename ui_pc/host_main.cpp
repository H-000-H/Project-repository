/*
 * PC 宿主: 开窗口 + 起 UI 线程, 让 app/ui 那层在 PC 上真跑起来。
 *   ui_host.exe                        开窗口跑, 按 ESC 或关窗退出
 *   ui_host.exe --hidden --ms 2000 --bmp pc_ui.bmp   跑 2 秒存图退出 (无人值守用)
 */
#include "app_ui.hpp"
#include "pc_display.h"
#include "pc_input.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    /* 崩溃现场: 打印异常地址与调用栈, 全部换算成 DLL 偏移 —— 事后用 nm 就能查符号 */
    LONG WINAPI crash_filter(EXCEPTION_POINTERS* info)
    {
        const char* dll_base = reinterpret_cast<const char*>(GetModuleHandleA("libui_logic.dll"));
        std::printf("\n[crash] code=0x%08lX addr=%p (dll+0x%llX)\n", info->ExceptionRecord->ExceptionCode,
                    info->ExceptionRecord->ExceptionAddress,
                    static_cast<unsigned long long>(
                        reinterpret_cast<const char*>(info->ExceptionRecord->ExceptionAddress) - dll_base));

        void*  frames[16] = {};
        USHORT n          = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
        for (USHORT i = 0; i < n; ++i)
        {
            std::printf("  frame[%u] %p (dll+0x%llX)\n", i, frames[i],
                        static_cast<unsigned long long>(reinterpret_cast<const char*>(frames[i]) - dll_base));
        }
        std::fflush(stdout);
        return EXCEPTION_EXECUTE_HANDLER;
    }
} // namespace

namespace
{
    const char* opt(int argc, char** argv, const char* key)
    {
        for (int i = 1; i < (argc - 1); ++i)
        {
            if (std::strcmp(argv[i], key) == 0)
            {
                return argv[i + 1];
            }
        }
        return nullptr;
    }

    bool has_flag(int argc, char** argv, const char* key)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], key) == 0)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    const bool        hidden = has_flag(argc, argv, "--hidden");
    const char*       ms_str = opt(argc, argv, "--ms");
    const char*       bmp    = opt(argc, argv, "--bmp");
    const std::uint32_t run_ms = (ms_str != nullptr) ? static_cast<std::uint32_t>(std::strtoul(ms_str, nullptr, 10)) : 0U;

    SetUnhandledExceptionFilter(crash_filter);

    std::printf("[ui_host] panel %dx%d, run_ms=%u, bmp=%s\n", kPcPanelWidth, kPcPanelHeight, run_ms,
                (bmp != nullptr) ? bmp : "(none)");

    pc_display_set_first_frame_hook(pc_input_attach_lvgl);
    pc_display_open(hidden);

    app::Ui ui("st7789", 10U);
    if (!ui.ThreadRegister())
    {
        std::printf("[ui_host] ui thread register failed\n");
        return 1;
    }

    pc_display_pump(run_ms, bmp);

    /* LVGL 内存池水位: max_used 是峰值, 也就是这一屏内容要过的最大一块 */
    {
        lv_mem_monitor_t mon{};
        lv_mem_monitor(&mon);
        std::printf("[ui_host] LVGL pool: total=%uKB free=%uKB max_used=%uKB (%u%%) frag=%u%%\n",
                    static_cast<unsigned>(mon.total_size / 1024U), static_cast<unsigned>(mon.free_size / 1024U),
                    static_cast<unsigned>(mon.max_used / 1024U), static_cast<unsigned>(mon.used_pct),
                    static_cast<unsigned>(mon.frag_pct));
    }

    pc_display_close();
    std::printf("[ui_host] exit\n");
    return 0;
}
