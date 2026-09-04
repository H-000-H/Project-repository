/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-C3 USB HAL 垫底 — C3 无 USB OTG (仅 USB-Serial/JTAG 下载调试口, 不走
 * TinyUSB DCD 设备栈), 板级 DTS 未启用 USB 设备时 bus/usb 的 DRIVER_REGISTER
 * 不会 probe。全屏蔽策略下提供 strong 占位避免链接缺失; 若经外挂 USB-PD 芯片
 * 等方案启用设备模式, 再在此接入 TinyUSB (docs/usb_tusb_port.md)。
 * HAL_USB_IMPL: hal_usb.h 对未实现时的调用方做 #pragma GCC poison, 实现方须先定义。
 */
#define HAL_USB_IMPL
#include "hal_usb.h"
#include "status.h"

int hal_usb_bus_host_init(struct hal_usb_bus_host* host, const struct hal_usb_bus_config* cfg)
{
    (void)host;
    (void)cfg;
    return MINI_ERR_NOTSUPP;
}

int hal_usb_bus_host_deinit(struct hal_usb_bus_host* host)
{
    (void)host;
    return MINI_ERR_NOTSUPP;
}

int hal_usb_irq_enable(const struct hal_usb_bus_host* host)
{
    (void)host;
    return MINI_ERR_NOTSUPP;
}

int hal_usb_irq_disable(const struct hal_usb_bus_host* host)
{
    (void)host;
    return MINI_ERR_NOTSUPP;
}

int hal_usb_resolve_xfer_mode(const struct hal_usb_bus_host* host, uint32_t xfer_mode)
{
    (void)host;
    (void)xfer_mode;
    return MINI_ERR_NOTSUPP;
}
