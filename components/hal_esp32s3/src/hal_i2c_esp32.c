/* SPDX-License-Identifier: Apache-2.0 */
/*
 * I2C HAL — ESP32-S3 实现 (Master)
 *
 * ESP 适配: cfg.i2c = I2C_NUM_*; scl/sda.pin = SoC GPIO; port/clk/af 不使用
 * DMA 路径返回 NOTSUPP（新 master API 内部调度，无 STM32 风格 DMA 句柄）
 */
#include "hal_i2c.h"
#include "status.h"
#include "compiler_compat.h"
#include "dt_config_gen.h"

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifndef DTC_GEN_I2C_HOST_MAX
#define DTC_GEN_I2C_HOST_MAX 3
#endif
#ifndef DTC_GEN_I2C_MAX_XFER
#define DTC_GEN_I2C_MAX_XFER 512U
#endif

#undef HAL_I2C_HOST_MAX
#define HAL_I2C_HOST_MAX DTC_GEN_I2C_HOST_MAX
#undef HAL_I2C_MAX_XFER
#define HAL_I2C_MAX_XFER DTC_GEN_I2C_MAX_XFER

#define ESP_I2C_DEV_CTX_MAX (HAL_I2C_HOST_MAX * 8)

struct esp_i2c_host_ctx
{
    i2c_master_bus_handle_t bus;
    uint8_t                 used;
};

struct esp_i2c_dev_ctx
{
    struct hal_i2c_dev*    dev;
    i2c_master_dev_handle_t handle;
    uint8_t                used;
};

static struct esp_i2c_host_ctx s_host_ctx[HAL_I2C_HOST_MAX];
static struct esp_i2c_dev_ctx  s_dev_ctx[ESP_I2C_DEV_CTX_MAX];

COMPAT_STATIC_INLINE int esp_i2c_map_err(esp_err_t err)
{
    if (err == ESP_OK)
        return VFS_OK;
    if (err == ESP_ERR_TIMEOUT)
        return VFS_ERR_TIMEOUT;
    if (err == ESP_ERR_INVALID_ARG)
        return VFS_ERR_INVAL;
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND)
        return VFS_ERR_IO;
    return VFS_ERR_IO;
}

COMPAT_STATIC_INLINE int esp_i2c_timeout(uint32_t timeout_ms)
{
    return timeout_ms ? (int)timeout_ms : 100;
}

static struct esp_i2c_host_ctx* esp_i2c_host_get(const struct hal_i2c_bus_host* host)
{
    if (!host || host->hw_idx < 0 || host->hw_idx >= HAL_I2C_HOST_MAX)
        return NULL;
    return &s_host_ctx[host->hw_idx];
}

static struct esp_i2c_dev_ctx* esp_i2c_dev_find(const struct hal_i2c_dev* dev)
{
    int i;

    if (!dev)
        return NULL;
    for (i = 0; i < ESP_I2C_DEV_CTX_MAX; i++)
    {
        if (s_dev_ctx[i].used && s_dev_ctx[i].dev == dev)
            return &s_dev_ctx[i];
    }
    return NULL;
}

static struct esp_i2c_dev_ctx* esp_i2c_dev_claim(struct hal_i2c_dev* dev)
{
    int i;

    if (!dev)
        return NULL;
    for (i = 0; i < ESP_I2C_DEV_CTX_MAX; i++)
    {
        if (!s_dev_ctx[i].used)
        {
            COMPAT_MEM_SET(&s_dev_ctx[i], 0, sizeof(s_dev_ctx[i]));
            s_dev_ctx[i].used = 1;
            s_dev_ctx[i].dev  = dev;
            return &s_dev_ctx[i];
        }
    }
    return NULL;
}

int hal_i2c_bus_host_init(struct hal_i2c_bus_host* host, int hw_idx, const struct hal_i2c_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= HAL_I2C_HOST_MAX)
        return VFS_ERR_INVAL;
    if (cfg->bus_role != HAL_I2C_BUS_ROLE_MASTER)
        return VFS_ERR_NOTSUPP;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg       = *cfg;
    host->i2c       = cfg->i2c;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return VFS_OK;
}

int hal_i2c_bus_host_deinit(struct hal_i2c_bus_host* host)
{
    struct esp_i2c_host_ctx* hctx;

    if (!host)
        return VFS_ERR_INVAL;

    hctx = esp_i2c_host_get(host);
    if (hctx && hctx->used && hctx->bus)
    {
        COMPAT_IGNORE_RESULT(i2c_del_master_bus(hctx->bus));
        COMPAT_MEM_SET(hctx, 0, sizeof(*hctx));
    }
    host->hw_inited = false;
    host->bus_ready = false;
    return VFS_OK;
}

int hal_i2c_dev_init(struct hal_i2c_dev* pdev, struct hal_i2c_bus_host* host, const struct hal_i2c_device_config* dev_cfg)
{
    if (!pdev || !host || !dev_cfg)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->ctlr = host;
    pdev->cfg  = *dev_cfg;
    return VFS_OK;
}

int hal_i2c_dev_deinit(struct hal_i2c_dev* pdev)
{
    if (!pdev)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    return VFS_OK;
}

int hal_i2c_dev_hw_open(struct hal_i2c_dev* dev)
{
    struct esp_i2c_host_ctx* hctx;
    struct esp_i2c_dev_ctx*  dctx;
    i2c_master_bus_config_t  bus_cfg;
    i2c_device_config_t      dev_cfg;
    esp_err_t                err;

    if (!dev || !dev->ctlr)
        return VFS_ERR_INVAL;
    if (dev->hw_open)
        return VFS_OK;

    hctx = esp_i2c_host_get(dev->ctlr);
    if (!hctx)
        return VFS_ERR_INVAL;

    if (!hctx->used)
    {
        COMPAT_MEM_SET(&bus_cfg, 0, sizeof(bus_cfg));
        bus_cfg.i2c_port                     = (i2c_port_num_t)dev->ctlr->cfg.i2c;
        bus_cfg.sda_io_num                   = (gpio_num_t)dev->ctlr->cfg.sda.pin;
        bus_cfg.scl_io_num                   = (gpio_num_t)dev->ctlr->cfg.scl.pin;
        bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt            = 7;
        bus_cfg.flags.enable_internal_pullup = 1;

        err = i2c_new_master_bus(&bus_cfg, &hctx->bus);
        if (err != ESP_OK)
            return esp_i2c_map_err(err);
        hctx->used              = 1;
        dev->ctlr->hw_inited    = true;
    }

    dctx = esp_i2c_dev_claim(dev);
    if (!dctx)
        return VFS_ERR_NOMEM;

    COMPAT_MEM_SET(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = (uint16_t)dev->cfg.address;
    dev_cfg.scl_speed_hz    = dev->cfg.clock_speed_hz ? dev->cfg.clock_speed_hz : 100000U;

    err = i2c_master_bus_add_device(hctx->bus, &dev_cfg, &dctx->handle);
    if (err != ESP_OK)
    {
        COMPAT_MEM_SET(dctx, 0, sizeof(*dctx));
        return esp_i2c_map_err(err);
    }

    dev->hw_open = 1;
    return VFS_OK;
}

int hal_i2c_dev_hw_close(struct hal_i2c_dev* pdev)
{
    struct esp_i2c_dev_ctx* dctx;

    if (!pdev)
        return VFS_ERR_INVAL;
    if (!pdev->hw_open)
        return VFS_OK;

    dctx = esp_i2c_dev_find(pdev);
    if (dctx && dctx->handle)
    {
        COMPAT_IGNORE_RESULT(i2c_master_bus_rm_device(dctx->handle));
        COMPAT_MEM_SET(dctx, 0, sizeof(*dctx));
    }
    pdev->hw_open = 0;
    return VFS_OK;
}

int hal_i2c_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    struct esp_i2c_dev_ctx* dctx;

    if (!pdev || !tx || len == 0 || len > HAL_I2C_MAX_XFER)
        return VFS_ERR_INVAL;
    dctx = esp_i2c_dev_find(pdev);
    if (!dctx || !dctx->handle)
        return VFS_ERR_NODEV;
    return esp_i2c_map_err(i2c_master_transmit(dctx->handle, tx, len, esp_i2c_timeout(timeout_ms)));
}

int hal_i2c_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    struct esp_i2c_dev_ctx* dctx;

    if (!pdev || !rx || len == 0 || len > HAL_I2C_MAX_XFER)
        return VFS_ERR_INVAL;
    dctx = esp_i2c_dev_find(pdev);
    if (!dctx || !dctx->handle)
        return VFS_ERR_NODEV;
    return esp_i2c_map_err(i2c_master_receive(dctx->handle, rx, len, esp_i2c_timeout(timeout_ms)));
}

int hal_i2c_sync(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    struct esp_i2c_dev_ctx* dctx;

    if (!pdev || len == 0 || len > HAL_I2C_MAX_XFER || (!tx && !rx))
        return VFS_ERR_INVAL;

    if (tx && !rx)
        return hal_i2c_write(pdev, tx, len, timeout_ms);
    if (!tx && rx)
        return hal_i2c_read(pdev, rx, len, timeout_ms);

    dctx = esp_i2c_dev_find(pdev);
    if (!dctx || !dctx->handle)
        return VFS_ERR_NODEV;
    return esp_i2c_map_err(i2c_master_transmit_receive(dctx->handle, tx, len, rx, len, esp_i2c_timeout(timeout_ms)));
}

int hal_i2c_dma_write(struct hal_i2c_dev* pdev, const uint8_t* tx, size_t len, uint32_t timeout_ms)
{
    (void)pdev;
    (void)tx;
    (void)len;
    (void)timeout_ms;
    return VFS_ERR_NOTSUPP;
}

int hal_i2c_dma_read(struct hal_i2c_dev* pdev, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    (void)pdev;
    (void)rx;
    (void)len;
    (void)timeout_ms;
    return VFS_ERR_NOTSUPP;
}

int hal_i2c_dma_write_then_read(struct hal_i2c_dev* pdev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    (void)pdev;
    (void)tx;
    (void)rx;
    (void)len;
    (void)timeout_ms;
    return VFS_ERR_NOTSUPP;
}
