/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file u8g2_port.c
 *@brief u8g2 胶水层实现 — u8x8 回调空透传 + 整帧缓冲经 DISPLAY_CMD_FLUSH 落屏
 *@author H-000-H
 *@details
 *   屏初始化序列与总线传输由 VFS 屏驱动承担 (probe/open + DISPLAY_CMD_*),
 *   u8g2 绘图只写本地整帧缓冲; 像素落屏路径:
 *   u8g2_port_flush → display_u8g2_flush_frame_buffer → device_ioctl(DISPLAY_CMD_FLUSH) → 屏驱动 →
 *bus → HAL。
 */

#include "u8g2_port.h"

#include "compiler_compat.h"
#include "display_ui_bridge.h"
#include "osal.h"
#include "status.h"
#include "system_log.h"

static const char* const s_kTag = "u8g2_port";

uint8_t u8g2_port_byte_callback(u8x8_t* u8x8_handle, uint8_t message, uint8_t argument_value,
                                void* argument_pointer)
{
    COMPAT_IGNORE_RESULT(u8x8_handle);
    COMPAT_IGNORE_RESULT(message);
    COMPAT_IGNORE_RESULT(argument_value);
    COMPAT_IGNORE_RESULT(argument_pointer);
    /* INIT / SET_DC / START_TRANSFER / SEND / END_TRANSFER 均空透传:
     * 屏初始化序列与总线传输语义由 VFS 屏驱动承担, 像素经 u8g2_port_flush 整帧落屏。 */
    return 1U;
}

uint8_t u8g2_port_gpio_and_delay_callback(u8x8_t* u8x8_handle, uint8_t message,
                                          uint8_t argument_value, void* argument_pointer)
{
    COMPAT_IGNORE_RESULT(u8x8_handle);
    COMPAT_IGNORE_RESULT(argument_pointer);
    switch (message)
    {
    case U8X8_MSG_DELAY_MILLI:
        osal_delay_ms(argument_value);
        break;
    case U8X8_MSG_DELAY_10MICRO:
        osal_delay_us((uint32_t)argument_value * 10U);
        break;
    case U8X8_MSG_DELAY_NANO:
        osal_delay_us((uint32_t)argument_value / 1000U + 1U);
        break;
    default:
        /* U8X8_MSG_GPIO_AND_DELAY_INIT / GPIO_CS / GPIO_DC / GPIO_RESET:
         * CS/DC/复位由 bus 层与屏驱动管理, 此处无操作。 */
        break;
    }
    return 1U;
}

int u8g2_port_attach(u8g2_t* u8g2_handle, struct device* display_device)
{
    int ret;

    if (!u8g2_handle || !display_device)
        return MINI_ERR_INVAL;
    ret = device_open(display_device, NULL);
    if (ret != MINI_OK)
    {
        SYS_LOGE(s_kTag, "device_open failed %d", ret);
        return ret;
    }
    u8g2_SetUserPtr(u8g2_handle, display_device);
    SYS_LOGI(s_kTag, "attach OK");
    return MINI_OK;
}

int u8g2_port_detach(u8g2_t* u8g2_handle)
{
    struct device* display_device;

    if (!u8g2_handle)
        return MINI_ERR_INVAL;
    display_device = (struct device*)u8g2_GetUserPtr(u8g2_handle);
    u8g2_SetUserPtr(u8g2_handle, NULL);
    if (!display_device)
        return MINI_ERR_NODEV;
    return device_close(display_device);
}

int u8g2_port_flush(u8g2_t* u8g2_handle, uint32_t timeout_ms)
{
    struct device* display_device;
    size_t buffer_length;

    if (!u8g2_handle)
        return MINI_ERR_INVAL;
    display_device = (struct device*)u8g2_GetUserPtr(u8g2_handle);
    if (!display_device)
        return MINI_ERR_NODEV;
    buffer_length = (size_t)u8g2_GetBufferTileWidth(u8g2_handle) *
                    (size_t)u8g2_GetBufferTileHeight(u8g2_handle) * 8U;
    if (timeout_ms == 0U)
        timeout_ms = CONFIG_UI_U8G2_FLUSH_TIMEOUT_MS;
    return display_u8g2_flush_frame_buffer(display_device, u8g2_GetBufferPtr(u8g2_handle),
                                           buffer_length, timeout_ms);
}
