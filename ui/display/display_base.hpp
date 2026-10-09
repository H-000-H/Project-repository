/**
 * @file display_base.hpp
 * @author H-000-H
 * @brief 屏幕刷新基类: 窗口可运行期设, 一帧交给捕获式回调刷
 * @note DeviceArg 是本屏 ioctl 入参类型, 由成员 arg_ 承载, 回调按引用拿(刷屏 lambda 捕获 0 字节);
 *       窗口不来自构造, 由 SetWindow 设; 回调捕获超 kFlushCapacity 编译期报错
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DISPLAY_BASE_HPP
#define DISPLAY_BASE_HPP
#include "device.h"
#include "status.h"
#include "system_log.h"
#include <etl/inplace_function.h>
#include <cstddef>
#include <cstdint>

namespace ui
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 日志 tag
    inline constexpr const char* kTag = "display";

    // 回调的定长存储(字节)
    inline constexpr std::size_t kFlushCapacity = 64U;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    /**
     * @brief 屏幕刷新基类: 窗口 + 一帧怎么交给驱动
     * @tparam color_depth 像素位宽(bit): 16 = RGB565, 1 = 单色
     * @tparam DeviceArg   本屏 ioctl 入参类型 (display_draw_arg / 别的命令集)
     * @tparam id          第几块屏
     */
    template <std::uint8_t color_depth, typename DeviceArg, std::uint8_t id = 0>
    class DisplayBase
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 刷屏实现: dev=面板, arg=本帧参数, timeout=超时 ms(0 = 驱动默认)
        using FlushCallback = etl::inplace_function<int(struct device*, DeviceArg& arg, std::uint32_t timeout), kFlushCapacity>;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 只绑定 device, 窗口先留空由 SetWindow 填 (label 查不到时 device_ptr 为 nullptr)
        explicit DisplayBase(const char* device_label): device_ptr((device_label != nullptr) ? device_find_by_label(device_label) : nullptr)
        {
            if (device_ptr == nullptr)
            {
                MT_LOG_ERROR(kTag, "device \"%s\" not found", (device_label != nullptr) ? device_label : "(null)");
            }
        }
        ~DisplayBase() = default;
        DisplayBase(const DisplayBase&)            = delete;
        DisplayBase& operator=(const DisplayBase&) = delete;
        DisplayBase(DisplayBase&&)                 = delete;
        DisplayBase& operator=(DisplayBase&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 设面板窗口(闭区间, 绝对坐标): 内部排序+校验; false = 非法, 成员保持原值
        bool SetWindow(std::uint16_t x1, std::uint16_t y1, std::uint16_t x2, std::uint16_t y2)
        {
            if (x1 > x2)
            {
                const std::uint16_t t = x1;
                x1                    = x2;
                x2                    = t;
                MT_LOG_ERROR(kTag, "x1 > x2, swapped");
            }
            if (y1 > y2)
            {
                const std::uint16_t t = y1;
                y1                    = y2;
                y2                    = t;
                MT_LOG_ERROR(kTag, "y1 > y2, swapped");
            }

            const std::uint32_t w = static_cast<std::uint32_t>(x2) - x1 + 1U;
            const std::uint32_t h = static_cast<std::uint32_t>(y2) - y1 + 1U;
            if ((w == 0U) || (h == 0U) || (w > 0xFFFFU) || (h > 0xFFFFU))
            {
                MT_LOG_ERROR(kTag, "bad window %ux%u", static_cast<unsigned>(w), static_cast<unsigned>(h));
                return false;
            }

            x_start = x1;
            y_start = y1;
            x_end   = x2;
            y_end   = y2;
            return true;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 窗口宽
        std::uint16_t Width() const { return static_cast<std::uint16_t>(x_end - x_start + 1U); }

        // 窗口高
        std::uint16_t Height() const { return static_cast<std::uint16_t>(y_end - y_start + 1U); }

        // 整屏字节数 (位宽 < 8 时向上取整)
        std::size_t CapacityBytes() const
        {
            return ((static_cast<std::size_t>(Width()) * Height() * color_depth) + 7U) / 8U;
        }

    protected:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 刷一帧: 派生类先填 arg_, 再调这里
        int flush(FlushCallback callback, DeviceArg& arg)
        {
            if (!callback)
            {
                MT_LOG_ERROR(kTag, "flush: empty callback");
                return MINI_ERR_INVAL;
            }
            cb = callback; /* 留一份当前实现(重刷/诊断用) */
            return cb(device_ptr, arg, flush_timeout);
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        DeviceArg      arg_{};   // 本屏 ioctl 入参, 每帧被派生类重填
        FlushCallback  cb;       // 留一份当前刷屏实现
        std::uint16_t  x_start = 0U; // 窗口(闭区间, 面板绝对坐标)
        std::uint16_t  y_start = 0U;
        std::uint16_t  x_end = 0U;
        std::uint16_t  y_end = 0U;
        ::device*      device_ptr    = nullptr; // 面板 device
        std::uint32_t  flush_timeout = 0U;      // 刷新超时 ms (0 = 驱动默认)
    };
} // namespace ui

#endif // DISPLAY_BASE_HPP
