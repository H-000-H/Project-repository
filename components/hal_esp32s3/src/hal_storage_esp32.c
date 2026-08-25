/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-S3 存储槽垫底 — 全屏蔽策略下 (mini_tree ESP 构建不编 weak stub) 的
 * strong 占位。返回 false (失败): config_store 走"读失败 → 回退工厂默认",
 * production_log 不加载历史 (s_ready 仍置位, 空状态)。
 * 注意: 必须返回 false 而非负错误码 — 调用方以 !ret 判定失败。
 * 需要持久化配置时在此接入 NVS (nvs_flash).
 */
#include "hal_storage.h"

bool hal_storage_init(void) { return false; }

bool hal_storage_read_flag(uint8_t* flag)
{
    (void)flag;
    return false;
}

bool hal_storage_write_flag(uint8_t flag)
{
    (void)flag;
    return false;
}

bool hal_storage_read_blob(uint8_t slot, uint8_t* buf, size_t* len)
{
    (void)slot;
    (void)buf;
    (void)len;
    return false;
}

bool hal_storage_write_blob(uint8_t slot, const uint8_t* buf, size_t len)
{
    (void)slot;
    (void)buf;
    (void)len;
    return false;
}

bool hal_storage_erase_all(void) { return false; }
