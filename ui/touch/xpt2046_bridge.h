/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file xpt2046_bridge.h
 *@brief XPT2046 ↔ LVGL indev 读点薄封装（不依赖 lvgl.h）
 *@author H-000-H
 *@details
 *   UI 胶水层: 应用把本函数适配进 LVGL indev read_cb,
 *   数据流: LVGL indev → xpt2046_lvgl_read → device_ioctl(XPT2046_CMD_READ_XY) → HAL。
 */

#ifndef XPT2046_BRIDGE_H
#define XPT2046_BRIDGE_H

#include "device.h"
#include "status.h"
#include "xpt2046_drv.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 读取坐标/按压状态（LVGL indev read_cb 用）
     * @param[in] touch_device XPT2046 device
     * @param[out] coordinate_result 输出坐标结果
     * @param[in] timeout_ms 超时（ms）
     * @return MINI_OK 或 VFS_ERR_*
     */
    COMPAT_STATIC_INLINE int xpt2046_lvgl_read(struct device* touch_device,
                                               struct xpt2046_xy* coordinate_result,
                                               uint32_t timeout_ms)
    {
        if (!touch_device || !coordinate_result)
            return MINI_ERR_INVAL;
        return device_ioctl(touch_device, XPT2046_CMD_READ_XY, coordinate_result,
                            sizeof(*coordinate_result), timeout_ms);
    }

#ifdef __cplusplus
}
#endif

#endif /* XPT2046_BRIDGE_H */
