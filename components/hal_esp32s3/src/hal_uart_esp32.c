/* SPDX-License-Identifier: Apache-2.0 */
/*
 * UART HAL — ESP32-S3 实现
 *
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 ESP-IDF uart driver。
 * - hal_uart_bus_host 嵌入 bus 层, HAL 无池管理无 vtable
 * - hw_open: uart_param_config + uart_set_pin + uart_driver_install (含事件队列)
 * - write/read 直接调 uart_write_bytes/uart_read_bytes
 * ESP32 适配: cfg.uart 承载 uart_port_t; tx/rx.pin 为 SoC GPIO 编号; port/clk/af 不使用
 * DMA 不支持 (无 STM32/WCH 风格 DMA API), 返回 VFS_ERR_NOTSUPP。
 */
#include "hal_uart.h"
#include "status.h"
#include "osal.h"
#include "compiler_compat.h"
#include "dt_config_gen.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/uart.h>
#include "esp_err.h"

#ifndef DTC_GEN_UART_MAX_XFER
#define DTC_GEN_UART_MAX_XFER 512U
#endif
#ifndef DTC_GEN_UART_TIMEOUT_MS
#define DTC_GEN_UART_TIMEOUT_MS 10U
#endif
#ifndef DTC_GEN_UART_EVENT_QUEUE
#define DTC_GEN_UART_EVENT_QUEUE 20U
#endif

#define ESP32_UART_RX_BUF DTC_GEN_UART_MAX_XFER
#define ESP32_UART_TX_BUF DTC_GEN_UART_MAX_XFER
#define ESP32_UART_EVENT_Q DTC_GEN_UART_EVENT_QUEUE
#define ESP32_UART_DEFAULT_TIMEOUT_MS DTC_GEN_UART_TIMEOUT_MS

COMPAT_STATIC_INLINE uint32_t esp32_uart_timeout_ms(uint32_t timeout_ms)
{
    return timeout_ms ? timeout_ms : ESP32_UART_DEFAULT_TIMEOUT_MS;
}

/*============================================================================*/
/*                              Device 管理 API                               */
/*============================================================================*/

int hal_uart_dev_init(struct hal_uart_bus_host* host, const struct hal_uart_config* cfg)
{
    if (!host || !cfg)
        return VFS_ERR_INVAL;

    COMPAT_MEM_SET(host, 0, sizeof(*host));
    host->cfg    = *cfg;
    host->uart   = cfg->uart;
    host->status = 0;
    return VFS_OK;
}

int hal_uart_dev_hw_open(struct hal_uart_bus_host* host)
{
    uart_config_t idf_cfg;
    QueueHandle_t queue = NULL;
    esp_err_t     err;

    if (!host || !host->cfg.uart)
        return VFS_ERR_INVAL;
    if (host->hw_inited)
        return VFS_OK;

    COMPAT_MEM_SET(&idf_cfg, 0, sizeof(idf_cfg));
    idf_cfg.baud_rate  = (int)host->cfg.baud_rate;
    idf_cfg.data_bits  = (uart_word_length_t)host->cfg.data_width;
    idf_cfg.stop_bits  = (uart_stop_bits_t)host->cfg.stop_bits;
    idf_cfg.parity     = (uart_parity_t)host->cfg.parity;
    idf_cfg.flow_ctrl  = host->cfg.hw_control ? (uart_hw_flowcontrol_t)host->cfg.hw_control : UART_HW_FLOWCTRL_DISABLE;
    idf_cfg.source_clk = UART_SCLK_DEFAULT;

    err = uart_param_config((uart_port_t)host->cfg.uart, &idf_cfg);
    if (err != ESP_OK)
        return VFS_ERR_IO;

    err = uart_set_pin((uart_port_t)host->cfg.uart,
                       (int)host->cfg.tx.pin,
                       (int)host->cfg.rx.pin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
        return VFS_ERR_IO;

    err = uart_driver_install((uart_port_t)host->cfg.uart,
                              ESP32_UART_RX_BUF,
                              ESP32_UART_TX_BUF,
                              ESP32_UART_EVENT_Q,
                              &queue,
                              ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK)
        return VFS_ERR_IO;

    host->uart       = host->cfg.uart;
    host->uart_queue = queue;
    host->hw_inited  = true;
    host->status     = 1;
    return VFS_OK;
}

int hal_uart_dev_hw_close(struct hal_uart_bus_host* host)
{
    if (!host || !host->cfg.uart)
        return VFS_ERR_INVAL;

    if (!host->hw_inited)
        return VFS_OK;

    if (uart_driver_delete((uart_port_t)host->cfg.uart) != ESP_OK)
        return VFS_ERR_IO;

    host->hw_inited  = false;
    host->uart_queue = NULL;
    host->status     = 0;
    return VFS_OK;
}

/*============================================================================*/
/*                              同步传输                                       */
/*============================================================================*/

int hal_uart_write(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct hal_uart_bus_host* host;
    int                       ret;

    COMPAT_IGNORE_RESULT(timeout_ms);
    if (!dev || !dev->ctlr || !data || len == 0)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    if (!host->hw_inited || !host->uart)
        return VFS_ERR_IO;

    host->status = 2;
    ret          = uart_write_bytes((uart_port_t)host->uart, (const char*)data, len);
    if (ret < 0 || (size_t)ret != len)
    {
        host->status = 3;
        return VFS_ERR_IO;
    }

    host->status = 1;
    return VFS_OK;
}

int hal_uart_read(struct hal_uart_dev* dev, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    struct hal_uart_bus_host* host;
    int                       read_len;

    if (!dev || !dev->ctlr || !data || len == 0)
        return VFS_ERR_INVAL;

    host = dev->ctlr;
    if (!host->hw_inited || !host->uart)
        return VFS_ERR_IO;

    host->status = 2;
    read_len     = uart_read_bytes((uart_port_t)host->uart,
                                   data,
                                   len,
                                   osal_timeout_to_ticks(esp32_uart_timeout_ms(timeout_ms)));
    if (read_len < 0)
    {
        host->status = 3;
        return VFS_ERR_IO;
    }
    if (read_len == 0)
    {
        host->status = 3;
        return VFS_ERR_TIMEOUT;
    }

    host->status = 1;
    return read_len;
}

/*============================================================================*/
/*                              DMA 传输 (ESP32 不支持)                        */
/*============================================================================*/

int hal_uart_write_dma(struct hal_uart_dev* dev, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    COMPAT_IGNORE_RESULT(dev);
    COMPAT_IGNORE_RESULT(data);
    COMPAT_IGNORE_RESULT(len);
    COMPAT_IGNORE_RESULT(timeout_ms);
    return VFS_ERR_NOTSUPP;
}

int hal_uart_dma_abort(struct hal_uart_dev* dev)
{
    COMPAT_IGNORE_RESULT(dev);
    return VFS_ERR_NOTSUPP;
}

int hal_virtual_uart_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(arg);
    COMPAT_IGNORE_RESULT(irq_num);
    return VFS_IRQ_ENTRY_NOBOTTOM;
}
