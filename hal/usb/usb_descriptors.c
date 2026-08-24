/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file usb_descriptors.c
 * @brief USB Device / Configuration / String 描述符
 *
 * OTG_FS 端点有限: Config0 = CDC+HID, Config1 = ECM, 主机择一枚举。
 */
#include "tusb.h"
#include <string.h>

#define USB_VID   0xCafe
#define USB_PID   0x4070
#define USB_BCD   0x0200

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_CDC_HID_TOTAL
};

enum {
    ITF_NUM_ECM = 0,
    ITF_NUM_ECM_DATA,
    ITF_NUM_ECM_TOTAL
};

enum {
    CONFIG_ID_CDC_HID = 0,
    CONFIG_ID_ECM     = 1,
    CONFIG_ID_COUNT
};

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82
#define EPNUM_HID_IN     0x83

#define EPNUM_ECM_NOTIF  0x81
#define EPNUM_ECM_OUT    0x02
#define EPNUM_ECM_IN     0x82

#define CONFIG_CDC_HID_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define CONFIG_ECM_LEN      (TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN)

//--------------------------------------------------------------------+
// Device
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = CONFIG_ID_COUNT
};

uint8_t const* tud_descriptor_device_cb(void)
{
    return (uint8_t const*)&desc_device;
}

//--------------------------------------------------------------------+
// HID report (generic in/out 64 — 简化为 vendor-like 8 字节)
//--------------------------------------------------------------------+
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE)
};

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

//--------------------------------------------------------------------+
// Configurations
//--------------------------------------------------------------------+
static uint8_t const desc_cfg_cdc_hid[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_CDC_HID + 1, ITF_NUM_CDC_HID_TOTAL, 0,
                          CONFIG_CDC_HID_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 5, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report),
                             0x04, EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 10),
};

static uint8_t const desc_cfg_ecm[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, ITF_NUM_ECM_TOTAL, 0,
                          CONFIG_ECM_LEN, 0, 100),
    TUD_CDC_ECM_DESCRIPTOR(ITF_NUM_ECM, 6, 7, EPNUM_ECM_NOTIF, 64,
                           EPNUM_ECM_OUT, EPNUM_ECM_IN, 64, CFG_TUD_NET_MTU),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index)
{
    if (index == CONFIG_ID_ECM)
        return desc_cfg_ecm;
    return desc_cfg_cdc_hid;
}

//--------------------------------------------------------------------+
// Strings
//--------------------------------------------------------------------+
static char const* string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "mini_tree",
    "STM32F407 USB",
    "40700001",
    "CDC ACM",
    "HID",
    "CDC ECM",
    "020304050607", /* MAC string for ECM (12 hex chars) */
};

static uint16_t _desc_str[32 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == 0)
    {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    }
    else
    {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;
        const char* str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 31)
            chr_count = 31;
        for (size_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = (uint16_t)(uint8_t)str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* HID weak stubs required by stack */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
