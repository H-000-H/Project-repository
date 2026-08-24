/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file usb_tusb_port.c
 * @brief TinyUSB 板级粘合实现
 * @note 仅包含 tusb.h; 不包含 mini_tree/osal.h。
 *       RNDIS 控制路径提供空 handler (本板描述符仅用 ECM)。
 */
#include "usb_tusb_port.h"
#include "tusb.h"

int usb_tusb_init(uint8_t rhport)
{
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    return tusb_init(rhport, &dev_init) ? 1 : 0;
}

void usb_tusb_task(void)
{
    tud_task();
}

void usb_tusb_int_handler(uint8_t rhport)
{
    tud_int_handler(rhport);
}

bool usb_tusb_cdc_connected(void)
{
    return tud_cdc_connected();
}

uint32_t usb_tusb_cdc_write(const void* buf, uint32_t len)
{
    return tud_cdc_write(buf, len);
}

void usb_tusb_cdc_write_flush(void)
{
    (void)tud_cdc_write_flush();
}

uint32_t usb_tusb_cdc_available(void)
{
    return tud_cdc_available();
}

uint32_t usb_tusb_cdc_read(void* buf, uint32_t len)
{
    return tud_cdc_read(buf, len);
}

bool usb_tusb_hid_ready(void)
{
    return tud_hid_ready();
}

int usb_tusb_hid_report(uint8_t report_id, const void* report, uint16_t len)
{
    return tud_hid_report(report_id, report, len) ? 1 : 0;
}

/*
 * TinyUSB ECM/RNDIS 驱动在 RNDIS 控制阶段会链接 rndis_class_set_handler。
 * 本板配置描述符仅使用 ECM, 此处提供空实现。
 */
void rndis_class_set_handler(uint8_t* data, int size)
{
    (void)data;
    (void)size;
}

