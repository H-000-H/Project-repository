/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file tusb_config.h
 * @brief TinyUSB 板级配置 — STM32F407 OTG_FS Device
 *
 * 启用 Device 栈与 CDC / HID / ECM class；主机默认枚举 Config0 (CDC+HID),
 * 日志调试用 CDC 虚拟串口。描述符与 Configuration 布局见 usb_descriptors.c。
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_STM32F4
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUH_ENABLED       0

#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_CDC           1
#define CFG_TUD_HID           1
#define CFG_TUD_ECM_RNDIS     1
#define CFG_TUD_NCM           0
#define CFG_TUD_MSC           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        0

#define CFG_TUD_CDC_NOTIFY    1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 64

#define CFG_TUD_HID_EP_BUFSIZE 16

#define CFG_TUD_NET_MTU       1514

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
