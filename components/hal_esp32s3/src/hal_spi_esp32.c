/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPI HAL — ESP32-S3 实现 (Master + Slave)
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 ESP-IDF spi_master/spi_slave driver。
 * - hal_spi_bus_host 嵌入 bus 层, host->hw_idx 索引 per-host dummy buffer
 * - cfg.spi 承载 spi_host_device_t; mosi/miso/sclk/cs.pin 为 SoC GPIO 编号
 * - dma_tx/dma_rx.dma_enable → SPI_DMA_CH_AUTO / SPI_DMA_DISABLED
 */
#include "hal_spi.h"
#include "status.h"
#include "osal.h"
#include "compiler_compat.h"
#include "dt_config_gen.h"

#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_err.h"

#include <stdatomic.h>

#ifndef DTC_GEN_SPI_HOST_MAX
#define DTC_GEN_SPI_HOST_MAX 4
#endif
#ifndef DTC_GEN_SPI_MAX_XFER
#define DTC_GEN_SPI_MAX_XFER 512U
#endif
#ifndef DTC_GEN_SPI_QUEUE_SIZE
#define DTC_GEN_SPI_QUEUE_SIZE 4U
#endif

#undef HAL_SPI_HOST_MAX
#define HAL_SPI_HOST_MAX DTC_GEN_SPI_HOST_MAX
#undef HAL_SPI_MAX_XFER
#define HAL_SPI_MAX_XFER DTC_GEN_SPI_MAX_XFER

#define ESP_SPI_QUEUE_SIZE DTC_GEN_SPI_QUEUE_SIZE
#define ESP_SPI_DEV_CTX_MAX (HAL_SPI_HOST_MAX * 4)

static uint8_t s_dummy_tx[HAL_SPI_HOST_MAX][HAL_SPI_MAX_XFER] COMPAT_ALIGNED(32);
static uint8_t s_dummy_rx[HAL_SPI_HOST_MAX][HAL_SPI_MAX_XFER] COMPAT_ALIGNED(32);

struct esp_spi_async_trans
{
    spi_transaction_t   idf_trans;
    hal_spi_callback_t  cb;
    void*               userdata;
    struct hal_spi_dev* dev;
    uint8_t             in_use;
};

struct esp_spi_dev_ctx
{
    struct hal_spi_dev*        dev;
    uint8_t                    used;
    uint8_t                    is_master;
    spi_device_handle_t        master;
    spi_slave_transaction_t    slave_trans;
    atomic_bool                slave_queued;
    uint8_t                    slave_tx[HAL_SPI_MAX_XFER] COMPAT_ALIGNED(32);
    uint8_t                    slave_dummy_rx[HAL_SPI_MAX_XFER] COMPAT_ALIGNED(32);
    struct esp_spi_async_trans async_pool[HAL_SPI_MAX_ASYNC];
};

static struct esp_spi_dev_ctx s_dev_ctx[ESP_SPI_DEV_CTX_MAX];

COMPAT_STATIC_INLINE spi_host_device_t esp_spi_host_id(const struct hal_spi_bus_host* host)
{
    return (spi_host_device_t)host->cfg.spi;
}

COMPAT_STATIC_INLINE spi_dma_chan_t esp_spi_dma_chan(const struct hal_spi_bus_config* cfg)
{
    if (cfg && (cfg->dma_tx.dma_enable || cfg->dma_rx.dma_enable))
        return SPI_DMA_CH_AUTO;
    return SPI_DMA_DISABLED;
}

COMPAT_STATIC_INLINE size_t esp_spi_max_xfer(const struct hal_spi_bus_host* host)
{
    if (!host)
        return (size_t)HAL_SPI_MAX_XFER;
    if (host->cfg.max_transfer_sz <= 0)
        return (size_t)HAL_SPI_MAX_XFER;
    return host->cfg.max_transfer_sz;
}

static struct esp_spi_dev_ctx* esp_spi_ctx_find(const struct hal_spi_dev* dev)
{
    int i;

    if (!dev)
        return NULL;

    for (i = 0; i < ESP_SPI_DEV_CTX_MAX; i++)
    {
        if (s_dev_ctx[i].used && s_dev_ctx[i].dev == dev)
            return &s_dev_ctx[i];
    }
    return NULL;
}

static struct esp_spi_dev_ctx* esp_spi_ctx_claim(struct hal_spi_dev* dev)
{
    int i;

    if (!dev || !dev->ctlr)
        return NULL;

    for (i = 0; i < ESP_SPI_DEV_CTX_MAX; i++)
    {
        if (!s_dev_ctx[i].used)
        {
            COMPAT_MEM_SET(&s_dev_ctx[i], 0, sizeof(s_dev_ctx[i]));
            s_dev_ctx[i].used      = 1;
            s_dev_ctx[i].dev       = dev;
            s_dev_ctx[i].is_master = (dev->ctlr->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER) ? 1U : 0U;
            atomic_store(&s_dev_ctx[i].slave_queued, false);
            return &s_dev_ctx[i];
        }
    }
    return NULL;
}

static void esp_spi_ctx_release(struct esp_spi_dev_ctx* ctx)
{
    if (!ctx)
        return;
    COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
}

static int esp_spi_master_bus_init(struct hal_spi_bus_host* host)
{
    const struct hal_spi_bus_config* bus_cfg;
    spi_bus_config_t                 idf_bus_cfg;

    if (!host || host->hw_inited)
        return VFS_OK;

    bus_cfg = &host->cfg;
    COMPAT_MEM_SET(&idf_bus_cfg, 0, sizeof(idf_bus_cfg));
    idf_bus_cfg.mosi_io_num     = (int)bus_cfg->mosi.pin;
    idf_bus_cfg.miso_io_num     = (int)bus_cfg->miso.pin;
    idf_bus_cfg.sclk_io_num     = (int)bus_cfg->sclk.pin;
    idf_bus_cfg.quadwp_io_num   = -1;
    idf_bus_cfg.quadhd_io_num   = -1;
    idf_bus_cfg.max_transfer_sz = (int)esp_spi_max_xfer(host);

    esp_err_t err = spi_bus_initialize(esp_spi_host_id(host), &idf_bus_cfg, esp_spi_dma_chan(bus_cfg));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return VFS_ERR_IO;

    host->hw_inited = true;
    return VFS_OK;
}

static int esp_spi_master_bus_deinit(struct hal_spi_bus_host* host)
{
    if (!host || !host->hw_inited)
        return VFS_OK;

    if (host->ref_count > 0)
        return VFS_ERR_BUSY;

    if (spi_bus_free(esp_spi_host_id(host)) != ESP_OK)
        return VFS_ERR_IO;

    host->hw_inited = false;
    COMPAT_MEM_SET(&host->active_cfg, 0, sizeof(host->active_cfg));
    return VFS_OK;
}

static void esp_spi_master_post_cb(spi_transaction_t* trans)
{
    struct esp_spi_async_trans* wrapper;

    if (!trans)
        return;

    wrapper = (struct esp_spi_async_trans*)trans->user;
    if (!wrapper || !wrapper->cb)
        return;

    wrapper->cb(wrapper->dev, trans, wrapper->userdata);
}

static int esp_spi_master_device_add(struct hal_spi_dev* dev, struct esp_spi_dev_ctx* ctx)
{
    spi_device_interface_config_t devcfg;
    esp_err_t                     err;

    if (!dev || !dev->ctlr || !ctx)
        return VFS_ERR_INVAL;

    if (ctx->master)
        return VFS_OK;

    COMPAT_MEM_SET(&devcfg, 0, sizeof(devcfg));
    devcfg.clock_speed_hz = dev->cfg.clock_speed_hz > 0 ? dev->cfg.clock_speed_hz : 1000000;
    devcfg.mode           = (uint8_t)dev->cfg.mode;
    devcfg.spics_io_num   = (int)dev->cfg.cs_pin;
    devcfg.queue_size     = ESP_SPI_QUEUE_SIZE;
    devcfg.post_cb        = esp_spi_master_post_cb;

    err = spi_bus_add_device(esp_spi_host_id(dev->ctlr), &devcfg, &ctx->master);
    if (err != ESP_OK)
        return VFS_ERR_IO;

    dev->ctlr->active_cfg = dev->cfg;
    return VFS_OK;
}

static int esp_spi_slave_hw_init(struct hal_spi_bus_host* host, const struct hal_spi_device_config* dev_cfg)
{
    spi_bus_config_t             idf_bus_cfg;
    spi_slave_interface_config_t slave_cfg;
    esp_err_t                    err;

    if (!host || !dev_cfg)
        return VFS_ERR_INVAL;
    if (host->hw_inited)
        return VFS_OK;

    COMPAT_MEM_SET(&idf_bus_cfg, 0, sizeof(idf_bus_cfg));
    idf_bus_cfg.mosi_io_num     = (int)host->cfg.mosi.pin;
    idf_bus_cfg.miso_io_num     = (int)host->cfg.miso.pin;
    idf_bus_cfg.sclk_io_num     = (int)host->cfg.sclk.pin;
    idf_bus_cfg.quadwp_io_num   = -1;
    idf_bus_cfg.quadhd_io_num   = -1;
    idf_bus_cfg.max_transfer_sz = (int)esp_spi_max_xfer(host);

    COMPAT_MEM_SET(&slave_cfg, 0, sizeof(slave_cfg));
    slave_cfg.spics_io_num = (int)dev_cfg->cs_pin;
    slave_cfg.mode         = (uint8_t)dev_cfg->mode;
    slave_cfg.queue_size   = ESP_SPI_QUEUE_SIZE;

    err = spi_slave_initialize(esp_spi_host_id(host), &idf_bus_cfg, &slave_cfg,
                               esp_spi_dma_chan(&host->cfg));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return VFS_ERR_IO;

    host->hw_inited  = true;
    host->active_cfg = *dev_cfg;
    return VFS_OK;
}

static int esp_spi_slave_hw_deinit(struct hal_spi_bus_host* host)
{
    spi_host_device_t idf_host;

    if (!host || !host->hw_inited)
        return VFS_OK;

    idf_host = esp_spi_host_id(host);
    COMPAT_IGNORE_RESULT(spi_slave_disable(idf_host));
    if (spi_slave_free(idf_host) != ESP_OK)
        return VFS_ERR_IO;

    host->hw_inited = false;
    COMPAT_MEM_SET(&host->active_cfg, 0, sizeof(host->active_cfg));
    return VFS_OK;
}

static int esp_spi_master_xfer(struct hal_spi_bus_host* host, struct esp_spi_dev_ctx* ctx, const uint8_t* tx, uint8_t* rx, size_t len)
{
    spi_transaction_t trans;
    const uint8_t*    tx_buf;
    uint8_t*          rx_buf;
    int               hw_idx;

    if (!host || !ctx || !ctx->master || len == 0 || len > HAL_SPI_MAX_XFER)
        return VFS_ERR_INVAL;

    hw_idx = host->hw_idx;
    if (hw_idx < 0 || hw_idx >= HAL_SPI_HOST_MAX)
        return VFS_ERR_INVAL;

    tx_buf = tx ? tx : s_dummy_tx[hw_idx];
    rx_buf = rx ? rx : s_dummy_rx[hw_idx];
    if (!tx)
        COMPAT_MEM_SET(s_dummy_tx[hw_idx], 0xFF, len);

    COMPAT_MEM_SET(&trans, 0, sizeof(trans));
    trans.length    = len * 8U;
    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    if (spi_device_transmit(ctx->master, &trans) != ESP_OK)
        return VFS_ERR_IO;

    return VFS_OK;
}

static int esp_spi_slave_setup_trans(struct esp_spi_dev_ctx* ctx, size_t len, const uint8_t* tx, uint8_t* rx_buf)
{
    if (!ctx || len == 0 || len > HAL_SPI_MAX_XFER)
        return VFS_ERR_INVAL;

    if (tx)
        __builtin_memcpy(ctx->slave_tx, tx, len);
    else
        COMPAT_MEM_SET(ctx->slave_tx, 0, len);

    COMPAT_MEM_SET(&ctx->slave_trans, 0, sizeof(ctx->slave_trans));
    ctx->slave_trans.length    = len * 8U;
    ctx->slave_trans.tx_buffer = ctx->slave_tx;
    ctx->slave_trans.rx_buffer = rx_buf;
    return VFS_OK;
}

static struct esp_spi_async_trans* esp_spi_async_alloc(struct esp_spi_dev_ctx* ctx)
{
    int i;

    if (!ctx)
        return NULL;

    for (i = 0; i < HAL_SPI_MAX_ASYNC; i++)
    {
        if (!ctx->async_pool[i].in_use)
        {
            ctx->async_pool[i].in_use = 1;
            return &ctx->async_pool[i];
        }
    }
    return NULL;
}

/*============================================================================*/
/*                              Host 管理 API                                 */
/*============================================================================*/

int hal_spi_bus_host_init(struct hal_spi_bus_host* host, int hw_idx, const struct hal_spi_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= HAL_SPI_HOST_MAX)
        return VFS_ERR_INVAL;
    if (host->bus_ready)
        return VFS_OK;
    if (!cfg->spi)
        return VFS_ERR_NODEV;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg = *cfg;

    if (host->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER &&
        host->cfg.bus_role != HAL_SPI_BUS_ROLE_SLAVE)
    {
        host->cfg.bus_role = HAL_SPI_BUS_ROLE_MASTER;
    }

    if (host->cfg.max_transfer_sz <= 0)
        host->cfg.max_transfer_sz = (size_t)HAL_SPI_MAX_XFER;
    else if (host->cfg.max_transfer_sz > (size_t)HAL_SPI_MAX_XFER)
        host->cfg.max_transfer_sz = (size_t)HAL_SPI_MAX_XFER;

    host->spi       = cfg->spi;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return VFS_OK;
}

int hal_spi_bus_host_deinit(struct hal_spi_bus_host* host)
{
    int ret = VFS_OK;

    if (!host)
        return VFS_ERR_INVAL;
    if (!host->bus_ready)
        return VFS_OK;
    if (host->ref_count > 0)
        return VFS_ERR_BUSY;

    if (host->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER)
        ret = esp_spi_master_bus_deinit(host);
    else
        ret = esp_spi_slave_hw_deinit(host);

    if (ret == VFS_OK)
        host->bus_ready = false;
    return ret;
}

/*============================================================================*/
/*                              Device 管理 API                               */
/*============================================================================*/

int hal_spi_dev_init(struct hal_spi_dev* dev, struct hal_spi_bus_host* host, const struct hal_spi_device_config* dev_cfg)
{
    if (!dev || !host || !dev_cfg)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(dev, 0, sizeof(*dev));
    dev->ctlr = host;
    dev->cfg  = *dev_cfg;
    return VFS_OK;
}

int hal_spi_dev_hw_open(struct hal_spi_dev* dev)
{
    struct hal_spi_bus_host* host;
    struct esp_spi_dev_ctx*  ctx;
    int                      ret;

    if (!dev || !dev->ctlr)
        return VFS_ERR_INVAL;
    if (dev->hw_open)
        return VFS_OK;

    host = dev->ctlr;
    if (!host->bus_ready)
        return VFS_ERR_INVAL;

    ctx = esp_spi_ctx_claim(dev);
    if (!ctx)
        return VFS_ERR_NOMEM;

    if (host->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER)
    {
        ret = esp_spi_master_bus_init(host);
        if (ret != VFS_OK)
            goto fail;

        ret = esp_spi_master_device_add(dev, ctx);
        if (ret != VFS_OK)
            goto fail;
    }
    else
    {
        if (host->hw_inited &&
            (host->active_cfg.cs_pin != dev->cfg.cs_pin ||
             host->active_cfg.mode != dev->cfg.mode))
        {
            ret = VFS_ERR_BUSY;
            goto fail;
        }

        ret = esp_spi_slave_hw_init(host, &dev->cfg);
        if (ret != VFS_OK)
            goto fail;
    }

    dev->hw_open = 1;
    host->ref_count++;
    return VFS_OK;

fail:
    esp_spi_ctx_release(ctx);
    return ret;
}

int hal_spi_dev_hw_close(struct hal_spi_dev* dev)
{
    struct hal_spi_bus_host* host;
    struct esp_spi_dev_ctx*  ctx;

    if (!dev || !dev->ctlr)
        return VFS_ERR_INVAL;
    if (!dev->hw_open)
        return VFS_OK;

    host = dev->ctlr;
    ctx  = esp_spi_ctx_find(dev);
    if (!host->bus_ready || host->ref_count <= 0 || !ctx)
        return VFS_ERR_INVAL;

    if (host->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER && ctx->master)
    {
        COMPAT_IGNORE_RESULT(spi_bus_remove_device(ctx->master));
        ctx->master = NULL;
    }

    host->ref_count--;
    esp_spi_ctx_release(ctx);
    dev->hw_open = 0;
    return VFS_OK;
}

/*============================================================================*/
/*                              Master 同步 / 异步                             */
/*============================================================================*/

int hal_spi_sync(struct hal_spi_dev* dev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms, uint32_t xfer_mode)
{
    struct hal_spi_bus_host* host;
    struct esp_spi_dev_ctx*  ctx;
    int                      ret;

    COMPAT_IGNORE_RESULT(timeout_ms);
    if (!dev || !dev->ctlr || !dev->hw_open || len == 0)
        return VFS_ERR_INVAL;
    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER)
        return VFS_ERR_INVAL;
    if (len > esp_spi_max_xfer(dev->ctlr))
        return VFS_ERR_INVAL;
    if (xfer_mode > HAL_SPI_XFER_DMA)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    ctx  = esp_spi_ctx_find(dev);
    if (!ctx || !ctx->master)
        return VFS_ERR_IO;

    if (xfer_mode == HAL_SPI_XFER_DMA &&
        !host->cfg.dma_tx.dma_enable && !host->cfg.dma_rx.dma_enable)
    {
        return VFS_ERR_NOTSUPP;
    }

    if (__builtin_memcmp(&host->active_cfg, &dev->cfg, sizeof(dev->cfg)) != 0)
    {
        COMPAT_IGNORE_RESULT(spi_bus_remove_device(ctx->master));
        ctx->master = NULL;
        ret         = esp_spi_master_device_add(dev, ctx);
        if (ret != VFS_OK)
            return ret;
    }

    return esp_spi_master_xfer(host, ctx, tx, rx, len);
}

int hal_spi_transfer_async(struct hal_spi_dev* dev, const uint8_t* tx, uint8_t* rx, size_t len, hal_spi_callback_t cb, void* userdata)
{
    struct esp_spi_dev_ctx*     ctx;
    struct esp_spi_async_trans* wrapper;

    if (!dev || !dev->ctlr || !dev->hw_open || len == 0)
        return VFS_ERR_INVAL;
    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER)
        return VFS_ERR_INVAL;
    if (len > esp_spi_max_xfer(dev->ctlr))
        return VFS_ERR_INVAL;

    ctx = esp_spi_ctx_find(dev);
    if (!ctx || !ctx->master)
        return VFS_ERR_IO;

    wrapper = esp_spi_async_alloc(ctx);
    if (!wrapper)
        return VFS_ERR_BUSY;

    COMPAT_MEM_SET(&wrapper->idf_trans, 0, sizeof(wrapper->idf_trans));
    wrapper->idf_trans.length    = len * 8U;
    wrapper->idf_trans.tx_buffer = tx;
    wrapper->idf_trans.rx_buffer = rx;
    wrapper->idf_trans.user      = wrapper;
    wrapper->cb                  = cb;
    wrapper->userdata            = userdata;
    wrapper->dev                 = dev;

    if (spi_device_queue_trans(ctx->master,
                               &wrapper->idf_trans,
                               osal_timeout_to_ticks(0)) != ESP_OK)
    {
        wrapper->in_use = 0;
        return VFS_ERR_IO;
    }

    return VFS_OK;
}

int hal_spi_transfer_poll(struct hal_spi_dev* dev, uint32_t timeout_ms)
{
    struct esp_spi_dev_ctx*     ctx;
    spi_transaction_t*          done = NULL;
    struct esp_spi_async_trans* wrapper;
    esp_err_t                   err;

    if (!dev || !dev->ctlr || !dev->hw_open)
        return VFS_ERR_INVAL;
    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_MASTER)
        return VFS_ERR_INVAL;

    ctx = esp_spi_ctx_find(dev);
    if (!ctx || !ctx->master)
        return VFS_ERR_IO;

    err = spi_device_get_trans_result(ctx->master, &done, osal_timeout_to_ticks(timeout_ms));
    if (err == ESP_ERR_TIMEOUT)
        return VFS_ERR_BUSY;
    if (err != ESP_OK || !done)
        return VFS_ERR_IO;

    wrapper = (struct esp_spi_async_trans*)done->user;
    if (wrapper)
        wrapper->in_use = 0;

    return VFS_OK;
}

/*============================================================================*/
/*                              Slave 传输                                    */
/*============================================================================*/

int hal_spi_slave_sync(struct hal_spi_dev* dev, const uint8_t* tx, uint8_t* rx, size_t len, uint32_t timeout_ms)
{
    struct esp_spi_dev_ctx* ctx;
    uint8_t*                rx_work;
    int                     ret;

    if (!dev || !dev->ctlr || !dev->hw_open || len == 0)
        return VFS_ERR_INVAL;
    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_SLAVE)
        return VFS_ERR_INVAL;
    if (!tx && !rx)
        return VFS_ERR_INVAL;
    if (len > esp_spi_max_xfer(dev->ctlr))
        return VFS_ERR_INVAL;

    ctx = esp_spi_ctx_find(dev);
    if (!ctx || !dev->ctlr->hw_inited)
        return VFS_ERR_IO;

    if (atomic_load_explicit(&ctx->slave_queued, memory_order_acquire))
        return VFS_ERR_BUSY;

    rx_work = rx ? rx : ctx->slave_dummy_rx;
    ret     = esp_spi_slave_setup_trans(ctx, len, tx, rx_work);
    if (ret != VFS_OK)
        return ret;

    if (spi_slave_transmit(esp_spi_host_id(dev->ctlr), &ctx->slave_trans,
                           osal_timeout_to_ticks(timeout_ms)) != ESP_OK)
    {
        return VFS_ERR_IO;
    }

    return VFS_OK;
}

int hal_spi_slave_queue_tx(struct hal_spi_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct esp_spi_dev_ctx* ctx;
    int                     ret;

    if (!dev || !dev->ctlr || !dev->hw_open || !data || len == 0)
        return VFS_ERR_INVAL;
    if (dev->ctlr->cfg.bus_role != HAL_SPI_BUS_ROLE_SLAVE)
        return VFS_ERR_INVAL;
    if (len > esp_spi_max_xfer(dev->ctlr))
        return VFS_ERR_INVAL;

    ctx = esp_spi_ctx_find(dev);
    if (!ctx || !dev->ctlr->hw_inited)
        return VFS_ERR_IO;

    if (atomic_load_explicit(&ctx->slave_queued, memory_order_acquire))
        return VFS_ERR_BUSY;

    ret = esp_spi_slave_setup_trans(ctx, len, data, ctx->slave_dummy_rx);
    if (ret != VFS_OK)
        return ret;

    if (spi_slave_queue_trans(esp_spi_host_id(dev->ctlr), &ctx->slave_trans,
                              osal_timeout_to_ticks(timeout_ms)) != ESP_OK)
    {
        return VFS_ERR_IO;
    }

    atomic_store_explicit(&ctx->slave_queued, true, memory_order_release);
    return VFS_OK;
}

int hal_spi_get_trans_result(struct hal_spi_dev* dev, uint8_t* rx_data, size_t rx_cap, size_t* trans_len, uint32_t timeout_ms)
{
    struct esp_spi_dev_ctx*  ctx;
    spi_slave_transaction_t* done = NULL;
    size_t                   rx_bytes;
    esp_err_t                err;

    ctx = esp_spi_ctx_find(dev);
    if (!dev || !dev->ctlr || !ctx || !dev->ctlr->hw_inited ||
        dev->ctlr->cfg.bus_role == HAL_SPI_BUS_ROLE_MASTER ||
        !atomic_load_explicit(&ctx->slave_queued, memory_order_acquire))
    {
        return VFS_ERR_INVAL;
    }

    err = spi_slave_get_trans_result(esp_spi_host_id(dev->ctlr), &done,
                                     osal_timeout_to_ticks(timeout_ms));
    if (err == ESP_ERR_TIMEOUT)
        return VFS_ERR_BUSY;
    if (err != ESP_OK || !done)
        return VFS_ERR_IO;

    atomic_store_explicit(&ctx->slave_queued, false, memory_order_release);

    rx_bytes = (done->trans_len + 7U) / 8U;
    if (trans_len)
        *trans_len = rx_bytes;

    if (rx_data && rx_cap > 0U && rx_bytes > 0U)
    {
        if (rx_bytes > rx_cap)
            return VFS_ERR_NOMEM;
        __builtin_memcpy(rx_data, ctx->slave_dummy_rx, rx_bytes < rx_cap ? rx_bytes : rx_cap);
    }

    return VFS_OK;
}

int hal_virtual_spi_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(arg);
    COMPAT_IGNORE_RESULT(irq_num);
    return VFS_IRQ_ENTRY_NOBOTTOM;
}
