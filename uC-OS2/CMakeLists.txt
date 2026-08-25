# ==============================================================================
# lib/uC-OS2 — Micrium uC/OS-II Kernel (V2.93.01, Apache-2.0, weston-embedded)
# ==============================================================================
# 端口: Ports/ARM-Cortex-M/ARMv7-M/GNU
# 配置: Cfg/os_cfg.h + app_cfg.h (项目自有副本, 1 ms tick / OS_MAX_EVENTS=24 /
#       OS_MAX_QS=8 / MBOX=MEM=FLAG=TMR 关闭)
# ==============================================================================

set(UCOS2_ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_library(ucos_ii_kernel STATIC
    # ── uC/OS-II 内核 (按 Cfg 开关裁剪) ──
    "Source/os_core.c"
    "Source/os_task.c"
    "Source/os_time.c"
    "Source/os_sem.c"
    "Source/os_mutex.c"
    "Source/os_q.c"
    "Source/os_dbg_r.c"     # OSInit 需要 OSDebugInit
    # ── ARMv7-M 端口 (GNU) ──
    "Ports/ARM-Cortex-M/ARMv7-M/os_cpu_c.c"
    "Ports/ARM-Cortex-M/ARMv7-M/GNU/os_cpu_a.S"
    "Ports/ARM-Cortex-M/ARMv7-M/GNU/os_dbg.c"
)

target_include_directories(ucos_ii_kernel
    PUBLIC
        "Source"
        "Cfg"
        "Ports/ARM-Cortex-M/ARMv7-M"
        "Ports/ARM-Cortex-M/ARMv7-M/GNU"
)
