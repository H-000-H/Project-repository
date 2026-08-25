/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file lvgl_port.h
 *@brief LVGL v9 胶水层 — lv_init / tick / display 创建与刷新全走 VFS 屏设备
 *@author H-000-H
 *@details
 *   @=========================================================================================================================*
 *   LVGL 胶水层 (ui/lvgl)
 *   架构位置: 应用 → [ui/lvgl (本文件)] → ui/display/display_ui_bridge.h →
 *device_ioctl(DISPLAY_CMD_*) → 屏驱动 → bus → HAL 职责:
 *   - lvgl_port_init(): lv_init + tick 源安装 (osal_time_ms, 经 lv_tick_set_cb);
 *   - lvgl_display_attach(): 打开屏 device → DISPLAY_CMD_GET_INFO 取面板几何/格式 →
 *     lv_display_create + 静态渲染缓冲 + flush 回调 (经 display_lvgl_flush_callback 落屏);
 *   - lvgl_port_task_handler(): 应用任务周期调用, 包装 lv_timer_handler。
 *   约束: 上下文池全静态 (CONFIG_UI_LVGL_DISPLAY_COUNT), 渲染缓冲由调用方静态提供, 无动态内存。
 *   @see ui/display/display_ui_bridge.h  UI 库 → DISPLAY_CMD_* 桥
 *   @see drivers/display/include/display_drv.h  屏统一命令
 *   @=========================================================================================================================
 */

#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "device.h"
#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief LVGL 显示胶水上下文（静态池分配, 容量 CONFIG_UI_LVGL_DISPLAY_COUNT） */
    struct lvgl_display_context
    {
        struct device* display_device; /**< VFS 屏设备 (display_drv.h 命令集) */
        lv_display_t* lvgl_display; /**< lv_display_create 返回的 LVGL 显示对象 */
        uint8_t pixel_format; /**< enum display_color_format, 取自 DISPLAY_CMD_GET_INFO */
        uint8_t in_use; /**< 池槽位占用标志 */
    };

    /**
     * @brief LVGL 内核初始化（幂等）: lv_init + tick 回调安装
     * @return 成功返回 MINI_OK
     */
    int lvgl_port_init(void);

    /**
     * @brief LVGL tick 源（毫秒）, 经 lv_tick_set_cb 安装
     * @return osal_time_ms 当前毫秒计数
     */
    uint32_t lvgl_port_tick_millisecond(void);

    /**
     * @brief 将 VFS 屏设备挂成 LVGL 显示对象
     * @param[in] display_device VFS 屏设备 (内部会 device_open)
     * @param[in] primary_buffer 渲染缓冲 1（调用方静态分配）
     * @param[in] secondary_buffer 渲染缓冲 2（可为 NULL, 单缓冲）
     * @param[in] buffer_size_bytes 单个渲染缓冲字节数
     * @return 成功返回上下文指针, 失败返回 NULL
     * @note 须先调用 lvgl_port_init()
     */
    struct lvgl_display_context* lvgl_display_attach(struct device* display_device,
                                                     uint8_t* primary_buffer,
                                                     uint8_t* secondary_buffer,
                                                     uint32_t buffer_size_bytes);

    /**
     * @brief 卸载显示对象: lv_display_delete + device_close + 归还池槽位
     * @param[in] display_context lvgl_display_attach 返回的上下文
     * @return 成功返回 MINI_OK, 失败返回负数错误码
     */
    int lvgl_display_detach(struct lvgl_display_context* display_context);

    /**
     * @brief LVGL 任务处理（应用任务周期调用, 包装 lv_timer_handler）
     * @return 距下次建议调用的毫秒数; 未初始化返回 0
     */
    uint32_t lvgl_port_task_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
