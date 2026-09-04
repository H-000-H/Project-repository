/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-C3 存储槽垫底 — 全屏蔽策略下 (mini_tree ESP 构建不编 weak stub) 的
 * strong 占位。1.4.5 的 hal_storage API 返回 MINI 错误码 (非 1.2.1 的 bool):
 * 返回 MINI_ERR_NOTSUPP, config_store 走"读失败 → 回退工厂默认",
 * production_log 不加载历史 (s_ready 仍置位, 空状态)。
 * 需要持久化配置时在此接入 NVS (nvs_flash)。
 */
#include "hal_storage.h"

int hal_storage_init(void) { return MINI_ERR_NOTSUPP; }

int hal_storage_read_flag(uint8_t* flag)
{
    (void)flag;
    return MINI_ERR_NOTSUPP;
}

int hal_storage_write_flag(uint8_t flag)
{
    (void)flag;
    return MINI_ERR_NOTSUPP;
}

int hal_storage_read_blob(uint8_t slot, uint8_t* buf, size_t* len)
{
    (void)slot;
    (void)buf;
    (void)len;
    return MINI_ERR_NOTSUPP;
}

int hal_storage_write_blob(uint8_t slot, const uint8_t* buf, size_t len)
{
    (void)slot;
    (void)buf;
    (void)len;
    return MINI_ERR_NOTSUPP;
}

int hal_storage_erase_all(void) { return MINI_ERR_NOTSUPP; }
