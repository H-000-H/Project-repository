/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file usb_net_cb.c
 * @brief CDC-ECM 网络回调与收发帧环缓
 *
 * 实现 TinyUSB tud_network_*_cb, 并向 usb_bus_ecm_* 提供 push/pop。
 */
#include "tusb.h"
#include "compiler_compat.h"
#include "status.h"

#include <string.h>

#ifndef USB_NET_FRAME_MAX
#define USB_NET_FRAME_MAX  CFG_TUD_NET_MTU
#endif
#ifndef USB_NET_RX_SLOTS
#define USB_NET_RX_SLOTS   4
#endif

uint8_t tud_network_mac_address[6] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

static uint8_t  s_rx[USB_NET_RX_SLOTS][USB_NET_FRAME_MAX];
static uint16_t s_rx_len[USB_NET_RX_SLOTS];
static uint8_t  s_rx_w, s_rx_r, s_rx_count;

static uint8_t  s_tx[USB_NET_FRAME_MAX];
static uint16_t s_tx_len;
static int      s_tx_pending;

void tud_network_init_cb(void)
{
    s_rx_w = s_rx_r = s_rx_count = 0;
    s_tx_len = 0;
    s_tx_pending = 0;
}

bool tud_network_recv_cb(const uint8_t* src, uint16_t size)
{
    if (!src || size == 0 || size > USB_NET_FRAME_MAX)
        return false;
    if (s_rx_count >= USB_NET_RX_SLOTS)
        return false;
    memcpy(s_rx[s_rx_w], src, size);
    s_rx_len[s_rx_w] = size;
    s_rx_w = (uint8_t)((s_rx_w + 1) % USB_NET_RX_SLOTS);
    s_rx_count++;
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg)
{
    (void)ref;
    (void)arg;
    if (!dst || !s_tx_pending || !s_tx_len)
        return 0;
    memcpy(dst, s_tx, s_tx_len);
    {
        uint16_t n = s_tx_len;
        s_tx_pending = 0;
        s_tx_len = 0;
        return n;
    }
}

int usb_net_frame_push_tx(const void* frame, size_t len)
{
    if (!frame || len == 0 || len > USB_NET_FRAME_MAX)
        return MINI_ERR_INVAL;
    if (s_tx_pending)
        return MINI_ERR_BUSY;
    if (!tud_ready() || !tud_network_can_xmit((uint16_t)len))
        return MINI_ERR_IO;
    memcpy(s_tx, frame, len);
    s_tx_len = (uint16_t)len;
    s_tx_pending = 1;
    tud_network_xmit(NULL, 0);
    return (int)len;
}

int usb_net_frame_pop_rx(void* frame, size_t len)
{
    uint16_t n;

    if (!frame || !len)
        return MINI_ERR_INVAL;
    if (s_rx_count == 0)
        return 0;
    n = s_rx_len[s_rx_r];
    if (n > len)
        return MINI_ERR_NOMEM;
    memcpy(frame, s_rx[s_rx_r], n);
    s_rx_r = (uint8_t)((s_rx_r + 1) % USB_NET_RX_SLOTS);
    s_rx_count--;
    tud_network_recv_renew();
    return (int)n;
}
