# ==============================================================================
# lib/uC-OS3 — Micrium uC/OS-III Kernel (V3.08.02, Apache-2.0, weston-embedded)
# ==============================================================================
# 依赖: lib/uC-CPU (cpu_core) + lib/uC-LIB (lib_def), 端口 Ports/ARM-Cortex-M/ARMv7-M/GNU
# 配置: Cfg/os_cfg.h + os_cfg_app.h (项目自有副本, DBG_EN=1 / MSG_POOL_SIZE=256)
# 裁剪: OS_CFG_FLAG_EN=MEM_EN=TMR_EN=0 (OSAL 不用), STAT_TASK_EN=1, APP_HOOKS_EN=1
# ==============================================================================

set(UCOS3_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(UCPU_ROOT "${UCOS3_ROOT}/../uC-CPU")
set(ULIB_ROOT "${UCOS3_ROOT}/../uC-LIB")

add_library(ucos_iii_kernel STATIC
    # ── uC/OS-III 内核 (按 Cfg 开关裁剪) ──
    "Source/os_core.c"
    "Source/os_task.c"
    "Source/os_tick.c"
    "Source/os_time.c"
    "Source/os_prio.c"
    "Source/os_var.c"
    "Source/os_msg.c"
    "Source/os_sem.c"
    "Source/os_mutex.c"
    "Source/os_q.c"
    "Source/os_stat.c"      # OS_CFG_STAT_TASK_EN=1
    "Source/os_dbg.c"       # OS_CFG_DBG_EN=1 (任务名进 TCB)
    "Source/os_cfg_app.c"   # OSCfg_* 常量与系统任务栈
    "Cfg/os_app_hooks.c"    # OS_CFG_APP_HOOKS_EN=1
    # ── ARMv7-M 端口 (GNU) ──
    "Ports/ARM-Cortex-M/ARMv7-M/os_cpu_c.c"
    "Ports/ARM-Cortex-M/ARMv7-M/GNU/os_cpu_a.S"
    # ── uC/CPU ──
    "${UCPU_ROOT}/cpu_core.c"
    "${UCPU_ROOT}/ARM-Cortex-M/ARMv7-M/cpu_c.c"
    "${UCPU_ROOT}/ARM-Cortex-M/ARMv7-M/GNU/cpu_a.s"
    # ── uC/LIB (cpu_core 依赖 Mem_Init) ──
    "${ULIB_ROOT}/lib_mem.c"
    "${ULIB_ROOT}/lib_str.c"
    "${ULIB_ROOT}/lib_ascii.c"
    "${ULIB_ROOT}/lib_math.c"
)

target_include_directories(ucos_iii_kernel
    PUBLIC
        "Source"
        "Cfg"
        "Ports/ARM-Cortex-M/ARMv7-M"
        "Ports/ARM-Cortex-M/ARMv7-M/GNU"
        "${UCPU_ROOT}"
        "${UCPU_ROOT}/Cfg"
        "${UCPU_ROOT}/ARM-Cortex-M/ARMv7-M"
        "${UCPU_ROOT}/ARM-Cortex-M/ARMv7-M/GNU"
        "${ULIB_ROOT}"
        "${ULIB_ROOT}/Cfg"
)
