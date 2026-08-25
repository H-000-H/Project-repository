/* SPDX-License-Identifier: Apache-2.0 */
/*
 * WS2812 应用任务：6 色循环，约每秒闪一次（亮/灭各半周期）
 * 传参优先引用；错误码/status + optional；不直调 HAL/SDK/内核任务 API
 */

#include "ws2812.hpp"

#include "device.h"
#include "status.h"
#include "ws2812_drv.h"
#include "task_manager.hpp"
#include "osal.h"
#include "system_log.h"
#include "etl/array.h"
#include "etl/optional.h"
#include "etl/span.h"
#include "etl/string_view.h"
#include <cstddef>
#include <cstdint>

namespace app::ws2812
{
/*============================================================================*/
constexpr etl::string_view kTag{"AppWs2812"};
constexpr etl::string_view kDevLabel{"ws2812_rgb"};

/* ETL_NO_STL 下 etl/chrono 拉入 std::strong_ordering 不可用，用命名 constexpr 代替散落魔数 */
constexpr uint32_t kHalfPeriodMs   = 500U;
constexpr uint32_t kIoctlTimeoutMs = 100U;
constexpr uint32_t kTaskStack      = 2048U;
constexpr uint32_t kTaskPriority   = 5U;
constexpr int      kTaskCore       = -1;

enum class ColorId : uint8_t
{
    Red = 0,
    Green,
    Blue,
    Yellow,
    Cyan,
    Magenta,
    Count
};

using Rgb = etl::array<uint8_t, 3>;

constexpr etl::array<Rgb, static_cast<size_t>(ColorId::Count)> kColors{{
    Rgb{16, 0, 0},    /* Red */
    Rgb{0, 16, 0},    /* Green */
    Rgb{0, 0, 16},    /* Blue */
    Rgb{16, 16, 0},   /* Yellow */
    Rgb{0, 16, 16},   /* Cyan */
    Rgb{16, 0, 16},   /* Magenta */
}};

/*============================================================================*/
etl::optional<int> set_rgb(struct device& dev, const Rgb& color)
{
    struct ws2812_rgb_arg arg{};

    arg.index = 0U;
    arg.r     = color[0];
    arg.g     = color[1];
    arg.b     = color[2];

    const int ret = device_ioctl(&dev, WS2812_CMD_SET_RGB, &arg, sizeof(arg), kIoctlTimeoutMs);
    if (ret != VFS_OK)
    {
        return etl::nullopt;
    }
    return ret;
}

etl::optional<int> clear_led(struct device& dev)
{
    const int ret = device_ioctl(&dev, WS2812_CMD_CLEAR, nullptr, 0U, kIoctlTimeoutMs);
    if (ret != VFS_OK)
    {
        return etl::nullopt;
    }
    return ret;
}

void task_entry(void* arg)
{
    auto&              dev = *static_cast<struct device*>(arg);
    etl::span<const Rgb> palette{kColors};
    size_t             idx = 0U;

    for (;;)
    {
        const Rgb& color = palette[idx];
        if (!set_rgb(dev, color))
        {
            SYS_LOGE(kTag.data(), "SET_RGB failed idx=%u", static_cast<unsigned>(idx));
        }
        osal_delay_ms(kHalfPeriodMs);

        if (!clear_led(dev))
        {
            SYS_LOGE(kTag.data(), "CLEAR failed");
        }
        osal_delay_ms(kHalfPeriodMs);

        idx = (idx + 1U) % palette.size();
    }
}

etl::optional<struct device*> open_device()
{
    struct device* dev = device_find_by_label(kDevLabel.data());
    if (IS_ERR(dev))
    {
        SYS_LOGW(kTag.data(), "device '%s' not found", kDevLabel.data());
        return etl::nullopt;
    }

    if (device_open(dev, nullptr) != VFS_OK)
    {
        SYS_LOGE(kTag.data(), "device_open failed");
        return etl::nullopt;
    }

    return dev;
}
} // namespace app::ws2812

etl::optional<int> app_ws2812_task_start()
{
    using namespace app::ws2812;

    etl::optional<struct device*> maybe_dev = open_device();
    if (!maybe_dev)
    {
        return etl::nullopt;
    }

    struct device& dev = **maybe_dev;

    const osal_task_handle_t th = TaskManager::create_task("ws2812", kTaskStack, kTaskPriority, task_entry, &dev, kTaskCore);
    if (!th)
    {
        SYS_LOGE(kTag.data(), "task create failed");
        const int close_ret = device_close(&dev);
        if (close_ret != VFS_OK)
        {
            SYS_LOGE(kTag.data(), "device_close failed: %d", close_ret);
        }
        return etl::nullopt;
    }

    SYS_LOGI(kTag.data(), "started, device=%s", device_get_name(&dev));
    return 0;
}
