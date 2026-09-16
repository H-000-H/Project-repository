/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file lvgl_port.hpp
 * @author H-000-H
 * @brief LVGL(v9) 屏适配: 把 LVGL 的 flush 回调接到 DisplayBase::Flush(lambda)
 */
#ifndef LVGL_PORT_HPP
#define LVGL_PORT_HPP
#include "display_base.hpp"
#include "status.h"
#include "system_log.h"
#include <cstddef>
#include <cstdint>
#include "lvgl.h"
#include <etl/array.h>
#ifndef LVGL_PORT_USE_DOUBLE_BUFFER
#define LVGL_PORT_USE_DOUBLE_BUFFER 0
#endif

namespace display
{
    class LvglPort : public DisplayBase
    {
    public:
        /** @brief 双缓冲(编译期): true 时第二块用本类的 m_flush_buffer_secondary, 否则传 nullptr */
        static constexpr bool kUseDoubleBuffer = (LVGL_PORT_USE_DOUBLE_BUFFER != 0);

    public:
        LvglPort(std::uint16_t height, std::uint16_t width, std::uint8_t bit_per_pixel,
                 const char* label, std::uint16_t occupy):DisplayBase(height, width, bit_per_pixel, label, occupy){}
        ~LvglPort() = default;

        LvglPort(const LvglPort&) = delete;
        LvglPort& operator=(const LvglPort&) = delete;
        LvglPort(LvglPort&&) = delete;
        LvglPort& operator=(LvglPort&&) = delete;
        lv_display_t*  disp      = nullptr;         /**< LVGL display 对象 (Init 里创建) */
        /** @brief 第二块绘制缓冲: 只在双缓冲时参与; 关掉时容量退化成 1 字节, 不占 RAM */
        etl::array<std::uint8_t, kUseDoubleBuffer ? kFlushBufferMaxBytes : 1U> m_flush_buffer_secondary{};
        static LvglPort& get_instance();
        /**
         * @brief 初始化屏幕 + 建 LVGL display + 挂 flush 回调 + 交绘制缓冲 (幂等, 可重复调)
         * @return MINI_OK 或 MINI_ERR_*
         * @note 绘制缓冲直接用基类的 m_flush_buffer(长度 m_flush_buffer_size), 本类不再另开一份;
         *       屏没 open 成功或参数检查不过时本函数会 device_close 回滚。
         */
        int Init();
        static void lvgl_port_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map); /**< LVGL flush 回调 */
    };

} // namespace display
#endif /* LVGL_PORT_HPP */