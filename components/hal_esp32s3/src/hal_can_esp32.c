/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CAN HAL — ESP32-S3 TWAI (新 esp_twai API)
 *
 * ESP 适配: cfg.can = 控制器索引; tx/rx.pin = GPIO;
 *           cfg.prescaler = bitrate Hz; mode 0/1/2 = normal/loopback/listen
 * RX: on_rx_done ISR 入队，hal_can_receive 阻塞出队
 */
#include "hal_can.h"
#include "status.h"
#include "compiler_compat.h"
#include "dt_config_gen.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_err.h"

#ifndef DTC_GEN_CAN_HOST_MAX
#define DTC_GEN_CAN_HOST_MAX 2
#endif

#undef HAL_CAN_HOST_MAX
#define HAL_CAN_HOST_MAX DTC_GEN_CAN_HOST_MAX

#define ESP_CAN_RX_Q_DEPTH 16

struct esp_can_rx_item
{
    struct can_frame frame;
};

struct esp_can_host_ctx
{
    twai_node_handle_t      node;
    QueueHandle_t           rx_q;
    struct hal_can_bus_host* host;
    uint8_t                 used;
};

static struct esp_can_host_ctx s_can_ctx[HAL_CAN_HOST_MAX];

COMPAT_STATIC_INLINE int esp_can_map_err(esp_err_t err)
{
    if (err == ESP_OK)
        return VFS_OK;
    if (err == ESP_ERR_TIMEOUT)
        return VFS_ERR_TIMEOUT;
    if (err == ESP_ERR_INVALID_ARG)
        return VFS_ERR_INVAL;
    if (err == ESP_ERR_INVALID_STATE)
        return VFS_ERR_BUSY;
    return VFS_ERR_IO;
}

static struct esp_can_host_ctx* esp_can_ctx_get(const struct hal_can_bus_host* host)
{
    if (!host || host->hw_idx < 0 || host->hw_idx >= HAL_CAN_HOST_MAX)
        return NULL;
    return &s_can_ctx[host->hw_idx];
}

static bool esp_can_on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t* edata, void* user_ctx)
{
    struct esp_can_host_ctx* ctx = (struct esp_can_host_ctx*)user_ctx;
    twai_frame_t             rx;
    uint8_t                  buf[CAN_MAX_DLEN];
    struct esp_can_rx_item   item;
    BaseType_t               hp = pdFALSE;

    (void)edata;
    if (!ctx || !ctx->rx_q)
        return false;

    COMPAT_MEM_SET(&rx, 0, sizeof(rx));
    COMPAT_MEM_SET(&item, 0, sizeof(item));
    rx.buffer     = buf;
    rx.buffer_len = sizeof(buf);

    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK)
        return false;

    item.frame.can_dlc = (uint8_t)(rx.header.dlc > CAN_MAX_DLEN ? CAN_MAX_DLEN : rx.header.dlc);
    item.frame.can_id  = rx.header.id & (rx.header.ide ? CAN_EFF_MASK : CAN_SFF_MASK);
    if (rx.header.ide)
        item.frame.can_id |= CAN_EFF_FLAG;
    if (rx.header.rtr)
        item.frame.can_id |= CAN_RTR_FLAG;
    __builtin_memcpy(item.frame.data, buf, item.frame.can_dlc);

    (void)xQueueSendFromISR(ctx->rx_q, &item, &hp);
    return hp == pdTRUE;
}

int hal_can_bus_host_init(struct hal_can_bus_host* host, int hw_idx, const struct hal_can_bus_config* cfg)
{
    if (!host || !cfg || hw_idx < 0 || hw_idx >= HAL_CAN_HOST_MAX)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg       = *cfg;
    host->can       = cfg->can;
    host->hw_idx    = hw_idx;
    host->bus_ready = true;
    return VFS_OK;
}

int hal_can_bus_host_deinit(struct hal_can_bus_host* host)
{
    struct esp_can_host_ctx* ctx;

    if (!host)
        return VFS_ERR_INVAL;

    ctx = esp_can_ctx_get(host);
    if (ctx && ctx->used)
    {
        if (ctx->node)
        {
            COMPAT_IGNORE_RESULT(twai_node_disable(ctx->node));
            COMPAT_IGNORE_RESULT(twai_node_delete(ctx->node));
        }
        if (ctx->rx_q)
            vQueueDelete(ctx->rx_q);
        COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
    }
    host->hw_inited = false;
    host->bus_ready = false;
    return VFS_OK;
}

int hal_can_dev_init(struct hal_can_dev* dev, struct hal_can_bus_host* host)
{
    if (!dev || !host)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(dev, 0, sizeof(*dev));
    dev->ctlr = host;
    return VFS_OK;
}

int hal_can_dev_deinit(struct hal_can_dev* dev)
{
    if (!dev)
        return VFS_ERR_INVAL;
    COMPAT_MEM_SET(dev, 0, sizeof(*dev));
    return VFS_OK;
}

int hal_can_dev_hw_open(struct hal_can_dev* dev)
{
    struct esp_can_host_ctx*   ctx;
    twai_onchip_node_config_t  ncfg;
    twai_event_callbacks_t     cbs;
    esp_err_t                  err;

    if (!dev || !dev->ctlr)
        return VFS_ERR_INVAL;
    if (dev->hw_open)
        return VFS_OK;

    ctx = esp_can_ctx_get(dev->ctlr);
    if (!ctx)
        return VFS_ERR_INVAL;

    if (!ctx->used)
    {
        COMPAT_MEM_SET(&ncfg, 0, sizeof(ncfg));
        ncfg.io_cfg.tx             = (gpio_num_t)dev->ctlr->cfg.tx.pin;
        ncfg.io_cfg.rx             = (gpio_num_t)dev->ctlr->cfg.rx.pin;
        ncfg.io_cfg.quanta_clk_out = -1;
        ncfg.io_cfg.bus_off_indicator = -1;
        ncfg.bit_timing.bitrate    = dev->ctlr->cfg.prescaler ? dev->ctlr->cfg.prescaler : 500000U;
        ncfg.bit_timing.sp_permill = (uint16_t)(dev->ctlr->cfg.bs1 ? dev->ctlr->cfg.bs1 : 875U);
        ncfg.tx_queue_depth        = 8;
        ncfg.fail_retry_cnt        = dev->ctlr->cfg.auto_retransmit ? -1 : 0;

        if (dev->ctlr->cfg.mode == 1U)
            ncfg.flags.enable_loopback = 1;
        else if (dev->ctlr->cfg.mode == 2U)
            ncfg.flags.enable_listen_only = 1;

        ctx->rx_q = xQueueCreate(ESP_CAN_RX_Q_DEPTH, sizeof(struct esp_can_rx_item));
        if (!ctx->rx_q)
            return VFS_ERR_NOMEM;

        err = twai_new_node_onchip(&ncfg, &ctx->node);
        if (err != ESP_OK)
        {
            vQueueDelete(ctx->rx_q);
            ctx->rx_q = NULL;
            return esp_can_map_err(err);
        }

        COMPAT_MEM_SET(&cbs, 0, sizeof(cbs));
        cbs.on_rx_done = esp_can_on_rx_done;
        err = twai_node_register_event_callbacks(ctx->node, &cbs, ctx);
        if (err != ESP_OK)
        {
            COMPAT_IGNORE_RESULT(twai_node_delete(ctx->node));
            vQueueDelete(ctx->rx_q);
            COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
            return esp_can_map_err(err);
        }

        err = twai_node_enable(ctx->node);
        if (err != ESP_OK)
        {
            COMPAT_IGNORE_RESULT(twai_node_delete(ctx->node));
            vQueueDelete(ctx->rx_q);
            COMPAT_MEM_SET(ctx, 0, sizeof(*ctx));
            return esp_can_map_err(err);
        }

        ctx->host = dev->ctlr;
        ctx->used = 1;
        dev->ctlr->hw_inited = true;
    }

    dev->hw_open = 1;
    return VFS_OK;
}

int hal_can_dev_hw_close(struct hal_can_dev* dev)
{
    if (!dev)
        return VFS_ERR_INVAL;
    dev->hw_open = 0;
    return VFS_OK;
}

int hal_can_transmit(struct hal_can_dev* dev, const struct can_frame* frame, uint32_t timeout_ms)
{
    struct esp_can_host_ctx* ctx;
    twai_frame_t             tx;
    twai_frame_header_t      hdr;
    uint8_t                  buf[CAN_MAX_DLEN];
    esp_err_t                err;

    if (!dev || !frame || !dev->ctlr)
        return VFS_ERR_INVAL;
    if (frame->can_id & CAN_ERR_FLAG)
        return VFS_ERR_INVAL;
    if (frame->can_dlc > CAN_MAX_DLEN)
        return VFS_ERR_INVAL;

    ctx = esp_can_ctx_get(dev->ctlr);
    if (!ctx || !ctx->node)
        return VFS_ERR_NODEV;

    COMPAT_MEM_SET(&hdr, 0, sizeof(hdr));
    hdr.id  = frame->can_id & ((frame->can_id & CAN_EFF_FLAG) ? CAN_EFF_MASK : CAN_SFF_MASK);
    hdr.ide = (frame->can_id & CAN_EFF_FLAG) ? 1 : 0;
    hdr.rtr = (frame->can_id & CAN_RTR_FLAG) ? 1 : 0;
    hdr.dlc = frame->can_dlc;
    __builtin_memcpy(buf, frame->data, frame->can_dlc);

    COMPAT_MEM_SET(&tx, 0, sizeof(tx));
    tx.header     = hdr;
    tx.buffer     = buf;
    tx.buffer_len = frame->can_dlc;

    err = twai_node_transmit(ctx->node, &tx, timeout_ms ? (int)timeout_ms : 100);
    return esp_can_map_err(err);
}

int hal_can_receive(struct hal_can_dev* dev, struct can_frame* frame, uint32_t fifo, uint32_t timeout_ms)
{
    struct esp_can_host_ctx* ctx;
    struct esp_can_rx_item   item;
    TickType_t               ticks;

    (void)fifo;
    if (!dev || !frame || !dev->ctlr)
        return VFS_ERR_INVAL;

    ctx = esp_can_ctx_get(dev->ctlr);
    if (!ctx || !ctx->rx_q)
        return VFS_ERR_NODEV;

    ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (xQueueReceive(ctx->rx_q, &item, ticks) != pdTRUE)
        return VFS_ERR_TIMEOUT;

    *frame = item.frame;
    return VFS_OK;
}

int hal_can_filter_config(struct hal_can_bus_host* host, const struct hal_can_filter_config* filter)
{
    struct esp_can_host_ctx*  ctx;
    twai_mask_filter_config_t mcfg;
    esp_err_t                 err;

    if (!host || !filter)
        return VFS_ERR_INVAL;

    ctx = esp_can_ctx_get(host);
    if (!ctx || !ctx->node)
        return VFS_ERR_NODEV;

    COMPAT_MEM_SET(&mcfg, 0, sizeof(mcfg));
    mcfg.id     = filter->id;
    mcfg.mask   = filter->mask;
    mcfg.is_ext = filter->ide ? true : false;

    /* 必须在 disable 状态配过滤；简化：尝试配置，失败则 NOTSUPP */
    COMPAT_IGNORE_RESULT(twai_node_disable(ctx->node));
    err = twai_node_config_mask_filter(ctx->node, (uint8_t)filter->bank, &mcfg);
    COMPAT_IGNORE_RESULT(twai_node_enable(ctx->node));
    return esp_can_map_err(err);
}

int hal_can_get_state(struct hal_can_bus_host* host, uint32_t* out_state)
{
    struct esp_can_host_ctx* ctx;
    twai_node_status_t       st;

    if (!host || !out_state)
        return VFS_ERR_INVAL;

    ctx = esp_can_ctx_get(host);
    if (!ctx || !ctx->node)
    {
        *out_state = HAL_CAN_STATE_STOPPED;
        return VFS_OK;
    }

    if (twai_node_get_info(ctx->node, &st, NULL) != ESP_OK)
        return VFS_ERR_IO;

    switch (st.state)
    {
    case TWAI_ERROR_ACTIVE:
        *out_state = HAL_CAN_STATE_ERROR_ACTIVE;
        break;
    case TWAI_ERROR_PASSIVE:
        *out_state = HAL_CAN_STATE_ERROR_PASSIVE;
        break;
    case TWAI_ERROR_BUS_OFF:
        *out_state = HAL_CAN_STATE_BUS_OFF;
        break;
    default:
        *out_state = HAL_CAN_STATE_STOPPED;
        break;
    }
    return VFS_OK;
}

int hal_virtual_can_irq_callback(void* arg, uint16_t irq_num)
{
    (void)arg;
    (void)irq_num;
    return VFS_OK;
}
