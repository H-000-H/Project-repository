/**
 * @file lvgl_port.hpp
 * @author H-000-H
 * @brief LVGL(v9) 屏适配: 单例 + 自带绘制缓冲, Init() 一次拉起整块屏
 * @note 应用层只填模板参并调 Init(); 刷屏: flush_cb → 填 arg_ → DISPLAY_CMD_FLUSH → 驱动,
 *       结尾必须 lv_display_flush_ready, 否则界面卡死
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef LVGL_PORT_HPP
#define LVGL_PORT_HPP
#include "compiler_compat.h"
#include "device.h"
#include "display_base.hpp"
#include "display_drv.h"
#include "mini_time.h"
#include "status.h"
#include "system_log.h"
#include "lvgl.h"
#include <etl/array.h>
#include <cstddef>
#include <cstdint>

namespace ui
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 单块绘制缓冲默认容量: 240x320 RGB565 分 10 块刷 = 15360B, 取 16KB
    inline constexpr std::uint32_t kDefaultFlushBufferBytes = 16384U;

    // 依赖模板参的 false: 让 static_assert 只在实例化时才对
    template <lv_color_format_t cf>
    inline constexpr bool kNoLvglFormatTraits = false;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // LVGL 色号 → (位宽, 驱动格式, 渲染模式); 没特化的色号 = 不支持
    template <lv_color_format_t cf>
    struct LvglFormatTraits
    {
        static_assert(kNoLvglFormatTraits<cf>,
                      "unsupported lv_color_format_t: 目前只支持 RGB565 / RGB565_SWAPPED / I1 / A1");
    };

    // RGB565: 16bpp, PARTIAL 分块刷
    template <>
    struct LvglFormatTraits<LV_COLOR_FORMAT_RGB565>
    {
        static constexpr std::uint8_t             bpp           = LV_COLOR_FORMAT_GET_BPP(LV_COLOR_FORMAT_RGB565);
        static constexpr std::uint8_t             driver_format = DISPLAY_FMT_RGB565;
        static constexpr lv_display_render_mode_t mode          = LV_DISPLAY_RENDER_MODE_PARTIAL;
    };

    // 只差 LVGL 侧字节序
    template <>
    struct LvglFormatTraits<LV_COLOR_FORMAT_RGB565_SWAPPED> : LvglFormatTraits<LV_COLOR_FORMAT_RGB565>
    {
    };

    // 1bpp 单色: 驱动只支持全屏刷
    template <>
    struct LvglFormatTraits<LV_COLOR_FORMAT_I1>
    {
        static constexpr std::uint8_t             bpp           = LV_COLOR_FORMAT_GET_BPP(LV_COLOR_FORMAT_I1);
        static constexpr std::uint8_t             driver_format = DISPLAY_FMT_MONO_1BPP;
        static constexpr lv_display_render_mode_t mode          = LV_DISPLAY_RENDER_MODE_FULL;
    };

    template <>
    struct LvglFormatTraits<LV_COLOR_FORMAT_A1> : LvglFormatTraits<LV_COLOR_FORMAT_I1>
    {
    };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    /**
     * @brief 屏适配
     * @tparam cf 色号 @tparam id 第几块屏 @tparam buffer_bytes 单块缓冲容量(字节)
     * @tparam double_buffer 双缓冲(该实例化 BSS 翻倍)
     * @note 构造私有: 唯一入口 get_instance(), 否则同 id 会有两个对象而回调只找单例
     */
    template <lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED, std::uint8_t id = 0U,
              std::uint32_t buffer_bytes = kDefaultFlushBufferBytes, bool double_buffer = false>
    class LvglPort : public DisplayBase<LvglFormatTraits<cf>::bpp, ::display_draw_arg, id>
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        using Base      = DisplayBase<LvglFormatTraits<cf>::bpp, ::display_draw_arg, id>;
        using DeviceArg = ::display_draw_arg;

        static constexpr lv_color_format_t        kColorFormat  = cf;
        static constexpr lv_display_render_mode_t kRenderMode   = LvglFormatTraits<cf>::mode;
        static constexpr std::uint8_t             kDriverFormat = LvglFormatTraits<cf>::driver_format;

        static_assert(buffer_bytes >= 4U, "绘制缓冲至少要 4 字节, 否则单帧长度算不出来");

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 唯一入口: 首调构造单例, 之后再调只取用; label=DTS label, occupy=整屏分几块刷
        static LvglPort& get_instance(const char* device_label, std::uint16_t occupy = 1U)
        {
            static LvglPort s_instance(device_label, occupy);
            self() = &s_instance;
            return s_instance;
        }

        // 不给参数时取实例
        static LvglPort& instance() { return *self(); }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 拉起整块屏: 开设备 + 要窗口 + 建 display + 挂回调 + 交缓冲; 幂等, 失败回滚
        int Init()
        {
            if (disp != nullptr)
            {
                return MINI_OK;
            }

            int ret = Open();
            if (ret != MINI_OK)
            {
                return ret;
            }

            const std::uint32_t slice = SliceBytes();
            if (slice == 0U)
            {
                MINI_IGNORE_RESULT(device_close(this->device_ptr));
                return MINI_ERR_NOTSUPP;
            }
            m_flush_buffer_size = slice;

            if (!lv_is_initialized())
            {
                lv_init();
                lv_tick_set_cb(mini_time_ms);
            }

            disp = lv_display_create(static_cast<int32_t>(this->Width()), static_cast<int32_t>(this->Height()));
            if (disp == nullptr)
            {
                MT_LOG_ERROR(kTag, "lv_display_create failed (LV_MEM_SIZE 不够?)");
                MINI_IGNORE_RESULT(device_close(this->device_ptr));
                return MINI_ERR_NOMEM;
            }

            lv_display_set_color_format(disp, kColorFormat);
            lv_display_set_flush_cb(disp, FlushEntry);
            lv_display_set_buffers(disp, m_flush_buffer.data(), double_buffer ? m_secondary.data() : nullptr, slice,
                                   kRenderMode);
            if (lv_display_get_default() == nullptr)
            {
                lv_display_set_default(disp);
            }

            MT_LOG_INFO(kTag, "id=%u display ready: %ux%u fmt=%u slice=%uB", static_cast<unsigned>(id),
                        static_cast<unsigned>(this->Width()), static_cast<unsigned>(this->Height()),
                        static_cast<unsigned>(kDriverFormat), static_cast<unsigned>(slice));
            return MINI_OK;
        }

        /**
         * @brief 运行期改窗口: 重算单帧长度 + 通知 LVGL 新分辨率/缓冲
         * @return MINI_ERR_INVAL 窗口非法; MINI_ERR_NOTSUPP 缓冲装不下
         * @note 只能在 UI 线程、非刷屏回调里调(换缓冲时那块内存正被 LVGL 用); 没建屏时只记窗口
         */
        int ApplyWindow(std::uint16_t x1, std::uint16_t y1, std::uint16_t x2, std::uint16_t y2)
        {
            if (!this->SetWindow(x1, y1, x2, y2))
            {
                return MINI_ERR_INVAL;
            }

            const std::uint32_t slice = SliceBytes();
            if (slice == 0U)
            {
                return MINI_ERR_NOTSUPP;
            }
            m_flush_buffer_size = slice;

            if (disp == nullptr)
            {
                return MINI_OK;
            }

            lv_display_set_resolution(disp, static_cast<int32_t>(this->Width()), static_cast<int32_t>(this->Height()));
            lv_display_set_buffers(disp, m_flush_buffer.data(), double_buffer ? m_secondary.data() : nullptr, slice,
                                   kRenderMode);
            lv_obj_invalidate(lv_display_get_screen_active(disp)); /* 新分辨率下让 LVGL 重排 root */

            MT_LOG_INFO(kTag, "id=%u window -> %ux%u slice=%uB", static_cast<unsigned>(id),
                        static_cast<unsigned>(this->Width()), static_cast<unsigned>(this->Height()),
                        static_cast<unsigned>(slice));
            return MINI_OK;
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // LVGL flush 入口(Init 里挂上): 结尾必须 flush_ready, 否则界面卡死
        static void FlushEntry(lv_display_t* display, const lv_area_t* area, uint8_t* px_map)
        {
            LvglPort* s = self();
            if (s == nullptr)
            {
                MT_LOG_ERROR(kTag, "flush: id=%u 还没 get_instance()", static_cast<unsigned>(id));
            }
            else
            {
                MINI_IGNORE_RESULT(s->LvglFlushCallback(display, area, px_map));
            }
            lv_display_flush_ready(display);
        }

        // 每帧: 填 arg_ 交给换屏实现
        int LvglFlushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* px_map)
        {
            (void)display;

            this->arg_ = DeviceArg{};
            /* 屏内坐标 → 面板绝对坐标 */
            this->arg_.x      = static_cast<int16_t>(this->x_start + area->x1);
            this->arg_.y      = static_cast<int16_t>(this->y_start + area->y1);
            this->arg_.w      = static_cast<int16_t>(area->x2 - area->x1 + 1);
            this->arg_.h      = static_cast<int16_t>(area->y2 - area->y1 + 1);
            this->arg_.format = kDriverFormat;
            this->arg_.data   = px_map;

            return this->flush([](struct device* dev, DeviceArg& a, std::uint32_t timeout) -> int
            {
                const int ret = device_ioctl(dev, DISPLAY_CMD_FLUSH, &a, sizeof(a), timeout);
                if (ret != MINI_OK)
                {
                    MT_LOG_ERROR(kTag, "flush %d,%d %dx%d failed: %d", static_cast<int>(a.x), static_cast<int>(a.y),
                                 static_cast<int>(a.w), static_cast<int>(a.h), ret);
                }
                return ret;
            }, this->arg_);
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        lv_display_t* disp = nullptr; // LVGL display (Init 里建)

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        LvglPort(const char* device_label, std::uint16_t occupy)
            : Base(device_label), m_occupy((occupy == 0U) ? 1U : occupy)
        {
        }
        ~LvglPort() = default;

        LvglPort(const LvglPort&)            = delete;
        LvglPort& operator=(const LvglPort&) = delete;
        LvglPort(LvglPort&&)                 = delete;
        LvglPort& operator=(LvglPort&&)      = delete;

        // 单例指针 (回调里取)
        static LvglPort*& self()
        {
            static LvglPort* s_self = nullptr;
            return s_self;
        }

        // 开设备 + 向驱动要窗口 (只给 Init 用, 免得出现"开了但没建屏"的半状态)
        int Open()
        {
            if (opened)
            {
                return MINI_OK;
            }
            if (this->device_ptr == nullptr)
            {
                MT_LOG_ERROR(kTag, "panel device is null (label 没查到, 构造时已报过)");
                return MINI_ERR_INVAL;
            }

            int ret = device_open(this->device_ptr, nullptr);
            if (ret != MINI_OK)
            {
                MT_LOG_ERROR(kTag, "panel open failed: %d", ret);
                return ret;
            }

            struct display_info_arg info = {0};
            ret = device_ioctl(this->device_ptr, DISPLAY_CMD_GET_INFO, &info, sizeof(info), 0);
            if (ret != MINI_OK)
            {
                MT_LOG_ERROR(kTag, "GET_INFO failed: %d", ret);
                MINI_IGNORE_RESULT(device_close(this->device_ptr));
                return ret;
            }
            if ((info.format != kDriverFormat) || (info.width == 0U) || (info.height == 0U))
            {
                MT_LOG_ERROR(kTag, "panel not usable: fmt=%u %ux%u (port fmt=%u)", static_cast<unsigned>(info.format),
                             static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
                             static_cast<unsigned>(kDriverFormat));
                MINI_IGNORE_RESULT(device_close(this->device_ptr));
                return MINI_ERR_NOTSUPP;
            }

            /* 窗口真值源 = 驱动(DTS), 代码里不写分辨率 */
            if (!this->SetWindow(0U, 0U, static_cast<std::uint16_t>(info.width - 1U),
                                 static_cast<std::uint16_t>(info.height - 1U)))
            {
                MINI_IGNORE_RESULT(device_close(this->device_ptr));
                return MINI_ERR_INVAL;
            }

            opened = true;
            MT_LOG_INFO(kTag, "id=%u panel %ux%u opened", static_cast<unsigned>(id), static_cast<unsigned>(this->Width()),
                        static_cast<unsigned>(this->Height()));
            return MINI_OK;
        }

        /**
         * @brief 单帧长度: 全屏刷=整屏, 分块刷=整屏/occupy, 4 字节对齐
         * @return 0 = 全屏刷装不下(已打日志); 分块刷装不下只夹到容量, LVGL 自己拆帧
         */
        std::uint32_t SliceBytes() const
        {
            const std::size_t raw = (kRenderMode == LV_DISPLAY_RENDER_MODE_FULL)
                                        ? this->CapacityBytes()
                                        : (this->CapacityBytes() / m_occupy);
            const std::size_t per_slice = (raw + 3U) & ~static_cast<std::size_t>(3U);

            if (per_slice == 0U)
            {
                MT_LOG_ERROR(kTag, "bad window: slice=0 (window %ux%u)", static_cast<unsigned>(this->Width()),
                             static_cast<unsigned>(this->Height()));
                return 0U;
            }

            if (kRenderMode == LV_DISPLAY_RENDER_MODE_FULL)
            {
                if (per_slice > buffer_bytes)
                {
                    MT_LOG_ERROR(kTag, "FULL render needs %uB, buffer=%uB", static_cast<unsigned>(per_slice),
                                 static_cast<unsigned>(buffer_bytes));
                    return 0U;
                }
                return static_cast<std::uint32_t>(per_slice);
            }

            if (per_slice > buffer_bytes)
            {
                MT_LOG_WARN(kTag, "slice %uB over buffer %uB (occupy=%u), clamped: 刷屏会变慢",
                            static_cast<unsigned>(per_slice), static_cast<unsigned>(buffer_bytes),
                            static_cast<unsigned>(m_occupy));
                return buffer_bytes;
            }
            return static_cast<std::uint32_t>(per_slice);
        }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        std::uint16_t m_occupy            = 1U; // 分块刷刷几块(全屏刷不看)
        std::uint32_t m_flush_buffer_size = 0U; // 单帧长度, Init 里算
        etl::array<std::uint8_t, buffer_bytes> m_flush_buffer{}; // 绘制缓冲
        etl::array<std::uint8_t, double_buffer ? buffer_bytes : 1U> m_secondary{}; // 第二块(双缓冲才参与)
        bool opened = false; // 是否已 open 并拿到窗口
    };
} // namespace ui

#endif // LVGL_PORT_HPP
