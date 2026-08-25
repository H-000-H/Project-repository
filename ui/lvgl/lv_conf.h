/* SPDX-License-Identifier: Apache-2.0 */
/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file lv_conf.h
 *@brief LVGL v9 移植配置 — ui/lvgl 胶水层专用
 *@author H-000-H
 *@details
 *   本文件经 cmake/lvgl.cmake 的 mini_tree_link_lvgl() 以 LV_BUILD_CONF_DIR 注入,
 *   lvgl 库与胶水层共用 (LV_CONF_INCLUDE_SIMPLE 方式解析)。
 *   只写覆盖项; 未覆盖项由 lv_conf_internal.h 的 #ifndef 默认值兜底。
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* ── OS 集成 ──
 * 不用 LVGL 内置 OS 层; 调度由应用任务周期调用 lvgl_port_task_handler() 完成,
 * tick 由 lvgl_port.c 经 lv_tick_set_cb(osal_time_ms) 提供。
 */
#define LV_USE_OS LV_OS_NONE

/* ── 渲染色彩深度 ──
 * 对齐 display_drv.h 的 DISPLAY_FMT_RGB565 (ST7789 等 RGB565 面板);
 * 单色屏场景由屏驱动在 DISPLAY_CMD_FLUSH 内做格式解析。
 */
#define LV_COLOR_DEPTH 16

/* ── 内存 ──
 * 内置分配器静态池 (LV_STDLIB_BUILTIN 为默认值, 此处显式声明池大小);
 * 禁止 LV_STDLIB_CLIB, 避免散落 malloc。
 */
#define LV_MEM_SIZE (32U * 1024U)

/* ── 杂项 ── */
#define LV_USE_LOG 0 /**< 日志走 mini_tree system_log, 不用 LVGL 内置日志 */
#define LV_DPI_DEF 130 /**< 默认 DPI (小屏仪表常用) */

#endif /* LV_CONF_H */
