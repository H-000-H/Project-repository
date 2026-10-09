/* SPDX-License-Identifier: Apache-2.0 */
/*
 * WS2812 驱动 — ESP32-S3 板载 RGB 灯珠
 *
 * 结构对齐 w25q64: DRIVER_REGISTER + file_operations + 池化 priv。
 * ioctl 与 VFS 一致: 命令映射表 O(1) 派发 (函数指针回调, 无 if/switch 链)。
 * gpio-pin / led-count 来自 DTS 零翻译透传。
 *
 * 【唯一例外 — 允许绕开 VFS】
 * 当前无 vfs-rmt：允许直接依赖 espressif/led_strip (RMT 后端)。
 * 这是唯一允许引用厂商 SDK 的产品驱动（其余在 mini_tree/drivers）；
 * 其余驱动禁止仿效。待 vfs-rmt 落地后迁移，删除本例外。
 */
#include "ws2812_drv.h"
#include "device.h"
#include "driver.h"
#include "dev_lifecycle.h"
#include "status.h"
#include "dt_config_gen.h"
#include "compiler_compat.h"
#include "mini_slot.h"
#include "system_log.h"

#include "led_strip.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>
#include "compiler_compat_poison.h"

#ifndef DTC_GEN_COUNT_WORLDSSEMI_WS2812
#define DTC_GEN_COUNT_WORLDSSEMI_WS2812  1
#endif

#define WS2812_COUNT           DTC_GEN_COUNT_WORLDSSEMI_WS2812
#define WS2812_RMT_RES_HZ      (10U * 1000U * 1000U)

struct ws2812_device
{
    struct file_operations ops;
    led_strip_handle_t     strip;
    int                    gpio_pin;
    int                    led_count;
    uint8_t                r;
    uint8_t                g;
    uint8_t                b;
};

static struct ws2812_device s_ws2812_pool[WS2812_COUNT] MINI_ALIGNED(4);
static uint8_t              s_ws2812_used[WS2812_COUNT] MINI_ALIGNED(4);
static mini_slot_t          s_ws2812_pool_ctrl MINI_ALIGNED(4);

static const char* const kTag = "ws2812";

mini_pre_execution(MINI_PRE_EXEC_PRIO_DRIVER_POOL)
static void ws2812_pool_boot_init(void)
{
    MINI_IGNORE_RESULT(mini_slot_init(&s_ws2812_pool_ctrl, s_ws2812_used, WS2812_COUNT));
}

static struct ws2812_device* ws2812_get_drvdata(struct device* pdev)
{
    return (struct ws2812_device*)device_get_priv(pdev);
}

static int ws2812_hw_create(struct ws2812_device* led)
{
    led_strip_config_t      strip_cfg;
    led_strip_rmt_config_t  rmt_cfg;
    esp_err_t               err;

    if (!led || led->gpio_pin < 0 || led->led_count <= 0)
    {
        return MINI_ERR_INVAL;
    }
    if (led->strip)
    {
        return MINI_OK;
    }

    MINI_MEM_SET(&strip_cfg, 0, sizeof(strip_cfg));
    strip_cfg.strip_gpio_num = led->gpio_pin;
    strip_cfg.max_leds       = (uint32_t)led->led_count;

    MINI_MEM_SET(&rmt_cfg, 0, sizeof(rmt_cfg));
    rmt_cfg.resolution_hz    = WS2812_RMT_RES_HZ;
    rmt_cfg.flags.with_dma   = false;

    err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led->strip);
    if (err != ESP_OK || !led->strip)
    {
        led->strip = NULL;
        return MINI_ERR_IO;
    }

    if (led_strip_clear(led->strip) != ESP_OK)
    {
        MINI_IGNORE_RESULT(led_strip_del(led->strip));
        led->strip = NULL;
        return MINI_ERR_IO;
    }

    led->r = 0;
    led->g = 0;
    led->b = 0;
    return MINI_OK;
}

static void ws2812_hw_destroy(struct ws2812_device* led)
{
    if (!led || !led->strip)
    {
        return;
    }
    MINI_IGNORE_RESULT(led_strip_clear(led->strip));
    MINI_IGNORE_RESULT(led_strip_del(led->strip));
    led->strip = NULL;
}

static int ws2812_open(struct device* pdev, void* arg)
{
    struct ws2812_device* led;
    struct dev_lifecycle* lc;
    int                   first;
    int                   ret;

    MINI_IGNORE_RESULT(arg);
    if (!pdev || !pdev->ops)
    {
        return MINI_ERR_INVAL;
    }

    led = ws2812_get_drvdata(pdev);
    if (IS_ERR(led))
    {
        return PTR_ERR(led);
    }

    lc = device_lc(pdev);
    if (IS_ERR(lc))
    {
        return PTR_ERR(lc);
    }

    first = dev_lc_open_begin(lc);
    if (first < 0)
    {
        return first;
    }

    ret = MINI_OK;
    if (first == 1)
    {
        ret = ws2812_hw_create(led);
        if (ret != MINI_OK)
        {
            dev_lc_open_abort(lc);
            return ret;
        }
    }

    dev_lc_open_end(lc);
    return MINI_OK;
}

static int ws2812_close(struct device* pdev)
{
    struct ws2812_device* led;
    struct dev_lifecycle* lc;
    int                   last;

    if (!pdev || !pdev->ops)
    {
        return MINI_ERR_INVAL;
    }

    led = ws2812_get_drvdata(pdev);
    if (IS_ERR(led))
    {
        return PTR_ERR(led);
    }

    lc = device_lc(pdev);
    if (IS_ERR(lc))
    {
        return PTR_ERR(lc);
    }

    last = dev_lc_close_begin(lc);
    if (last < 0)
    {
        return last;
    }

    if (last)
    {
        ws2812_hw_destroy(led);
    }

    dev_lc_close_end(lc);
    return MINI_OK;
}

/*============================================================================*/
/* ioctl 命令映射表 — index = (cmd - WS2812_CMD_BASE - 1)                     */
/*============================================================================*/
typedef int (*ws2812_ioctl_fn_t)(struct ws2812_device* led, void* arg,
                                 size_t arg_len, uint32_t timeout_ms);

struct ws2812_ioctl_map
{
    ws2812_ioctl_fn_t handler;
};

static int ws2812_cmd_set_rgb(struct ws2812_device* led, void* arg, size_t arg_len, uint32_t timeout_ms)
{
    const struct ws2812_rgb_arg* rgb = (const struct ws2812_rgb_arg*)arg;

    MINI_IGNORE_RESULT(timeout_ms);
    if (!led || !led->strip || !rgb || arg_len != sizeof(*rgb))
    {
        return MINI_ERR_INVAL;
    }
    if (rgb->index >= (uint32_t)led->led_count)
    {
        return MINI_ERR_INVAL;
    }

    if (led_strip_set_pixel(led->strip, rgb->index, rgb->r, rgb->g, rgb->b) != ESP_OK)
    {
        return MINI_ERR_IO;
    }
    if (led_strip_refresh(led->strip) != ESP_OK)
    {
        return MINI_ERR_IO;
    }

    if (rgb->index == 0U)
    {
        led->r = rgb->r;
        led->g = rgb->g;
        led->b = rgb->b;
    }
    return MINI_OK;
}

static int ws2812_cmd_clear(struct ws2812_device* led, void* arg, size_t arg_len, uint32_t timeout_ms)
{
    MINI_IGNORE_RESULT(arg);
    MINI_IGNORE_RESULT(arg_len);
    MINI_IGNORE_RESULT(timeout_ms);

    if (!led || !led->strip)
    {
        return MINI_ERR_INVAL;
    }
    if (led_strip_clear(led->strip) != ESP_OK)
    {
        return MINI_ERR_IO;
    }

    led->r = 0;
    led->g = 0;
    led->b = 0;
    return MINI_OK;
}

static int ws2812_cmd_get_rgb(struct ws2812_device* led, void* arg, size_t arg_len, uint32_t timeout_ms)
{
    struct ws2812_rgb_arg* rgb = (struct ws2812_rgb_arg*)arg;

    MINI_IGNORE_RESULT(timeout_ms);
    if (!led || !rgb || arg_len != sizeof(*rgb))
    {
        return MINI_ERR_INVAL;
    }
    if (rgb->index >= (uint32_t)led->led_count)
    {
        return MINI_ERR_INVAL;
    }

    /* 板载单灯缓存 index0；多灯未缓存时仅允许读 0 */
    if (rgb->index != 0U)
    {
        return MINI_ERR_NOTSUPP;
    }

    rgb->r = led->r;
    rgb->g = led->g;
    rgb->b = led->b;
    return MINI_OK;
}

static const struct ws2812_ioctl_map s_ws2812_ioctl_map[WS2812_CMD_COUNT] =
{
    [WS2812_CMD_SET_RGB - WS2812_CMD_BASE - 1] = { ws2812_cmd_set_rgb },
    [WS2812_CMD_CLEAR - WS2812_CMD_BASE - 1]   = { ws2812_cmd_clear },
    [WS2812_CMD_GET_RGB - WS2812_CMD_BASE - 1] = { ws2812_cmd_get_rgb },
};

static int ws2812_ioctl(struct device* pdev, int cmd, void* arg, size_t arg_len, uint32_t timeout_ms)
{
    struct ws2812_device* led;
    struct dev_lifecycle* lc;
    int32_t               offset;
    int                   ret;

    if (!pdev || !pdev->ops)
    {
        return MINI_ERR_INVAL;
    }

    led = ws2812_get_drvdata(pdev);
    if (IS_ERR(led))
    {
        return PTR_ERR(led);
    }

    lc = device_lc(pdev);
    if (IS_ERR(lc))
    {
        return PTR_ERR(lc);
    }

    ret = dev_lc_io_begin(lc);
    if (ret != MINI_OK)
    {
        return ret;
    }

    offset = (int32_t)cmd - (int32_t)WS2812_CMD_BASE;
    if (offset < 1 || offset > WS2812_CMD_COUNT ||
        !s_ws2812_ioctl_map[offset - 1].handler)
    {
        ret = MINI_ERR_INVAL;
    }
    else
    {
        ret = s_ws2812_ioctl_map[offset - 1].handler(led, arg, arg_len, timeout_ms);
    }

    dev_lc_io_end(lc);
    return ret;
}

static const struct file_operations ws2812_fops =
{
    .open  = ws2812_open,
    .close = ws2812_close,
    .ioctl = ws2812_ioctl,
};

static int ws2812_probe(struct device* pdev)
{
    struct ws2812_device* led;
    int                   pin_val = -1;
    int                   count_val = 1;
    int                   pool_idx;
    int                   ret;

    if (!pdev)
    {
        return MINI_ERR_INVAL;
    }

    pool_idx = mini_slot_claim(&s_ws2812_pool_ctrl);
    if (pool_idx < 0)
    {
        return MINI_ERR_NOMEM;
    }

    led = &s_ws2812_pool[pool_idx];
    MINI_MEM_SET(led, 0, sizeof(*led));

    if (device_get_prop_int(pdev, "gpio-pin", &pin_val) != MINI_OK)
    {
        ret = MINI_ERR_INVAL;
        goto err_pool;
    }

    MINI_IGNORE_RESULT(device_get_prop_int(pdev, "led-count", &count_val));
    if (count_val <= 0)
    {
        count_val = 1;
    }
    if (count_val > (int)WS2812_LED_MAX)
    {
        count_val = (int)WS2812_LED_MAX;
    }

    led->gpio_pin  = pin_val;
    led->led_count = count_val;
    led->strip     = NULL;

    if (device_set_priv(pdev, led) != MINI_OK)
    {
        ret = MINI_ERR_IO;
        goto err_pool;
    }

    led->ops = ws2812_fops;
    pdev->ops = &led->ops;

    MT_LOG_INFO(kTag, "probe OK: pool=%d gpio=%d leds=%d", pool_idx, pin_val, count_val);
    return MINI_OK;

err_pool:
    pdev->ops = NULL;
    MINI_MEM_SET(led, 0, sizeof(*led));
    MINI_IGNORE_RESULT(mini_slot_release(&s_ws2812_pool_ctrl, pool_idx));
    return ret;
}

static int ws2812_remove(struct device* pdev)
{
    struct ws2812_device* led;
    struct dev_lifecycle* lc;
    int                   pool_idx;

    if (!pdev)
    {
        return MINI_ERR_INVAL;
    }

    led = ws2812_get_drvdata(pdev);
    if (IS_ERR(led))
    {
        return PTR_ERR(led);
    }

    lc = device_lc(pdev);
    if (IS_ERR(lc))
    {
        return PTR_ERR(lc);
    }

    pool_idx = (int)(led - s_ws2812_pool);

    dev_lc_remove_start(lc);
    device_ops_unregister(pdev);

    if (dev_lc_remove_drain(lc, MINI_WAIT_FOREVER) != MINI_OK)
    {
        dev_lc_remove_finish(lc);
        return MINI_ERR_IO;
    }

    ws2812_hw_destroy(led);
    MINI_MEM_SET(led, 0, sizeof(*led));
    MINI_IGNORE_RESULT(mini_slot_release(&s_ws2812_pool_ctrl, pool_idx));
    dev_lc_remove_finish(lc);
    return MINI_OK;
}

DRIVER_REGISTER(ws2812, "worldsemi,ws2812", ws2812_probe, ws2812_remove)
