/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-S3 Flash 访问垫底 — 全屏蔽策略下 (mini_tree ESP 构建不编 weak stub) 的
 * strong 占位。返回 false/0 (失败): system_scrubber (默认关) 不会空推进 offset。
 * 需要 Flash CRC 巡检时在此接入 esp_partition/spi_flash 真实实现。
 */
#include "hal_flash.h"
#include <stddef.h>

bool hal_flash_read(uint32_t addr, uint8_t* buf, size_t len)
{
    (void)addr;
    (void)buf;
    (void)len;
    return false;
}

uint32_t hal_flash_get_app_addr(void) { return 0; }

uint32_t hal_flash_get_app_size(void) { return 0; }
