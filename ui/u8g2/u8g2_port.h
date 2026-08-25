/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file u8g2_port.h
 *@brief u8g2 胶水层 — u8x8 回调与整帧缓冲刷新全走 VFS 屏设备
 *@author H-000-H
 *@details
 *   @=========================================================================================================================*
 *   u8g2 胶水层 (ui/u8g2)
 *   架构位置: 应用 → [ui/u8g2 (本文件)] → ui/display/display_ui_bridge.h →
 *device_ioctl(DISPLAY_CMD_*) → 屏驱动 → bus → HAL 用法 (整帧缓冲模式, 即各面板的 u8g2_Setup_*_f
 *初始化过程):
 *   - u8g2_Setup_*_f(u8g2_handle, rotation, u8g2_port_byte_callback,
 *u8g2_port_gpio_and_delay_callback);
 *   - u8g2_port_attach(u8g2_handle, display_device): 打开屏 device 并绑定 user_ptr;
 *   - u8g2 绘图后调 u8g2_port_flush(u8g2_handle, timeout): 整帧 page-major 缓冲经
 *     display_u8g2_flush_frame_buffer → DISPLAY_CMD_FLUSH 落屏。
 *   说明: 屏初始化序列与总线传输语义由 VFS 屏驱动 (probe/open + DISPLAY_CMD_*) 承担,
 *   u8g2 字节流只写入本地整帧缓冲, 故 byte 回调为空透传; CS/DC/复位由 bus 层管理。
 *   @see ui/display/display_ui_bridge.h  UI 库 → DISPLAY_CMD_* 桥
 *   @see drivers/display/include/display_drv.h  屏统一命令
 *   @=========================================================================================================================
 */

#ifndef U8G2_PORT_H
#define U8G2_PORT_H

#include "device.h"
#include "u8g2.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief u8x8 字节回调（传给 u8g2_Setup_* 的 byte_cb）
     * @param[in] u8x8_handle u8x8 上下文
     * @param[in] message U8X8_MSG_BYTE_* 消息
     * @param[in] argument_value 消息参数（字节数/电平）
     * @param[in] argument_pointer 消息数据指针
     * @return 恒返回 1（成功）: 像素不经字节流落屏, 见文件头说明
     */
    uint8_t u8g2_port_byte_callback(u8x8_t* u8x8_handle, uint8_t message, uint8_t argument_value,
                                    void* argument_pointer);

    /**
     * @brief u8x8 GPIO/延时回调（传给 u8g2_Setup_* 的 gpio_and_delay_cb）
     * @param[in] u8x8_handle u8x8 上下文
     * @param[in] message U8X8_MSG_GPIO_* / U8X8_MSG_DELAY_* 消息
     * @param[in] argument_value 消息参数（延时量/电平）
     * @param[in] argument_pointer 消息数据指针
     * @return 恒返回 1（成功）: 延时走 osal, GPIO 由 bus 层管理
     */
    uint8_t u8g2_port_gpio_and_delay_callback(u8x8_t* u8x8_handle, uint8_t message,
                                              uint8_t argument_value, void* argument_pointer);

    /**
     * @brief 绑定 VFS 屏设备: device_open + u8g2_SetUserPtr
     * @param[in] u8g2_handle 已完成 u8g2_Setup_*_f 的句柄
     * @param[in] display_device VFS 屏设备 (display_drv.h 命令集, 仅支持单色面板)
     * @return 成功返回 MINI_OK, 失败返回负数错误码
     */
    int u8g2_port_attach(u8g2_t* u8g2_handle, struct device* display_device);

    /**
     * @brief 解绑: 清 user_ptr + device_close
     * @param[in] u8g2_handle 已 attach 的句柄
     * @return 成功返回 MINI_OK, 失败返回负数错误码
     */
    int u8g2_port_detach(u8g2_t* u8g2_handle);

    /**
     * @brief 整帧缓冲刷新（绘图完成后调用）
     * @param[in] u8g2_handle 已 attach 的句柄（须为 _f 整帧缓冲初始化）
     * @param[in] timeout_ms 超时（ms）; 0 表示用 CONFIG_UI_U8G2_FLUSH_TIMEOUT_MS
     * @return 成功返回 MINI_OK, 失败返回负数错误码
     */
    int u8g2_port_flush(u8g2_t* u8g2_handle, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* U8G2_PORT_H */
