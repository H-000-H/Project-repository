/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file ft5x06_bridge.h
 *@brief FT5x06 ↔ LVGL indev 读点薄封装（不依赖 lvgl.h）
 *@author H-000-H
 *@details
 *   UI 胶水层: 应用把本函数适配进 LVGL indev read_cb,
 *   数据流: LVGL indev → ft5x06_lvgl_read → device_ioctl(FT5X06_CMD_READ_TOUCH) → HAL。
 */

#ifndef FT5X06_BRIDGE_H
#define FT5X06_BRIDGE_H

#include "device.h"
#include "ft5x06_drv.h"
#include "status.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 读取触摸点（LVGL indev read_cb 用）
     * @param[in] touch_device FT5x06 device
     * @param[out] touch_result 输出触摸结果
     * @param[in] timeout_ms 超时（ms）
     * @return MINI_OK 或 VFS_ERR_*
     */
    COMPAT_STATIC_INLINE int ft5x06_lvgl_read(struct device* touch_device,
                                              struct ft5x06_touch* touch_result,
                                              uint32_t timeout_ms)
    {
        if (!touch_device || !touch_result)
            return MINI_ERR_INVAL;
        return device_ioctl(touch_device, FT5X06_CMD_READ_TOUCH, touch_result,
                            sizeof(*touch_result), timeout_ms);
    }

#ifdef __cplusplus
}
#endif

#endif /* FT5X06_BRIDGE_H */
