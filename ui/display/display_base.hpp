/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file display_base.hpp
 * @author H-000-H
 * @brief 屏幕刷新基类: 统一走 lambda(捕获式)回调, 不分配堆
 * @details
 *   回调存在 etl::inplace_function 的固定容量存储里, 所以调用方可以直接写捕获 lambda
 *   ([this] / [&dev] 都行), 既不需要虚函数也不需要堆。捕获体积超过 kFlushCapacity
 *   会在编译期 static_assert 报错, 不会静默丢捕获。
 *   本头只 include 自己用到的头; 定长缓冲容器 etl::array 由用到它的派生类自行 include。
 */
#ifndef DISPLAY_BASE_HPP
#define DISPLAY_BASE_HPP
#include "device.h"
#include "etl/algorithm.h"
#include "system_log.h"
#include <cstddef>
#include <cstdint>
#include <etl/array.h>
#include <etl/inplace_function.h>
namespace display
{
/** @brief 日志 tag */
inline constexpr const char* kTag = "display";
/** @brief 回调可调用对象的固定存储(字节); 捕获超了是编译期报错, 不会静默丢捕获 */
inline constexpr std::uint16_t kFlushCapacity = 64U;
/** @brief 单块缓冲上限(字节): 32KB = 240px 宽 x 68 行的 RGB565; etl::array 容量必须编译期确定 */
inline constexpr std::uint32_t kFlushBufferMaxBytes = 32768U;

class DisplayBase
{
public:
    using DisplayFlushCallback = etl::inplace_function<int(struct device* dev, std::size_t timeout), kFlushCapacity>;

    void Flush(DisplayFlushCallback callback)
    {
        if (!callback)
        {
            MT_LOG_ERROR(kTag, "flush: empty callback");
            return;
        }
        cb = etl::move(callback); /* 留一份当前实现(重刷/诊断用) */
        (void)cb(m_device, m_flush_timeout);
    }
    /**
     * @brief 绑定面板 device 并算出单块刷新缓冲大小
     * @param[in] height,width 面板像素
     * @param[in] bit_per_pixel 像素位宽 (16 = RGB565, 1 = 单色)
     * @param[in] label DTS label; nullptr 或查不到时 m_device 为 nullptr 并打日志
     * @param[in] occupy 整屏分几块刷 (0 非法, 按 1 处理)
     */
    DisplayBase(std::uint16_t height, std::uint16_t width, std::uint8_t bit_per_pixel,
                const char* label, std::uint16_t occupy)
        : m_device((label != nullptr) ? device_find_by_label(label) : nullptr),
          m_occupy((occupy == 0U) ? 1U : occupy)
    {
        if (m_device == nullptr)
        {
            MT_LOG_ERROR(kTag, "device \"%s\" not found", (label != nullptr) ? label : "(null)");
        }

        const std::uint32_t total_bits      = static_cast<std::uint32_t>(width) * height * bit_per_pixel;
        const std::uint32_t total_bytes     = (total_bits + 7U) / 8U; /* bpp < 8 (单色) 时向上取整 */
        const std::uint32_t per_slice_bytes = ((total_bytes / m_occupy) + 3U) & ~3U;
        if (per_slice_bytes > kFlushBufferMaxBytes)
        {
            MT_LOG_ERROR(kTag, "flush buffer %u over max %u, clamped", static_cast<unsigned>(per_slice_bytes),
                         static_cast<unsigned>(kFlushBufferMaxBytes));
            m_flush_buffer_size = kFlushBufferMaxBytes;
        }
        else
        {
            m_flush_buffer_size = (per_slice_bytes != 0U) ? per_slice_bytes : 4U;
        }
    }

protected:
    ::device*     m_device            = nullptr; /**< 面板 device; label 查不到时为 nullptr */
    std::uint16_t m_flush_timeout     = 0U;      /**< 刷新超时 ms (0 = 交给驱动默认) */
    std::uint32_t m_flush_buffer_size = 0U;      /**< 本屏单块缓冲实际长度 (<= kFlushBufferMaxBytes) */
    std::uint16_t m_occupy            = 1U;      /**< 整屏分几块刷 (0 已夹紧为 1) */
    /** @brief 绘制缓冲: 给 LVGL 的 lv_display_set_buffers 用(PARTIAL 分块), Attach 时接上 */
    etl::array<std::uint8_t, kFlushBufferMaxBytes> m_flush_buffer{};
    DisplayFlushCallback cb;                     /**< 已注册的刷屏实现 (空 = 上层漏了 Flush(lambda)) */
};

} // namespace display

#endif /* DISPLAY_BASE_HPP */