/**
 * SPDX-License-Identifier: Apache-2.0
 * @file ws2812_drv.h
 * @brief WS2812 驱动 — 板载可寻址 RGB 灯珠应用层接口
 * @note open → ioctl(SET_RGB / CLEAR / GET_RGB) → close
 */
#ifndef WS2812_DRV_H
#define WS2812_DRV_H

#include <stddef.h>
#include <stdint.h>
#include "compiler_compat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WS2812_LED_MAX               8U

/*
 *   open → ioctl(SET_RGB) → ioctl(CLEAR) → ioctl(GET_RGB) → close
 */
#define WS2812_CMD_BASE              COMPAT_MAGIC(WS2812)
#define WS2812_CMD_SET_RGB           (WS2812_CMD_BASE + 0x01)
#define WS2812_CMD_CLEAR             (WS2812_CMD_BASE + 0x02)
#define WS2812_CMD_GET_RGB           (WS2812_CMD_BASE + 0x03)
#define WS2812_CMD_COUNT             3

struct ws2812_rgb_arg
{
    uint32_t index;  /**< 灯珠序号, 板载单灯用 0 */
    uint8_t  r;      /**< 红 0..255 */
    uint8_t  g;      /**< 绿 0..255 */
    uint8_t  b;      /**< 蓝 0..255 */
};

#ifdef __cplusplus
}
#endif

#endif /* WS2812_DRV_H */
