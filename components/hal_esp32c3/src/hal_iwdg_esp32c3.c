/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-C3 IWDG 垫底 — ESP-IDF 看门狗走 TWDT (见 sdkconfig CONFIG_ESP_TASK_WDT /
 * system_wdt.cpp placeholder 分支), mini_tree 的 IWDG 抽象不接硬件。
 * 全屏蔽策略下 (mini_tree ESP 构建不编 weak stub, 见 esp_idf.cmake HAL_SRCS 注释)
 * 本文件提供 strong 占位, 缺失会直接链接报错。
 */
#include "hal_iwdg.h"
#include "status.h"

int hal_iwdg_init(struct hal_iwdg_dev* pdev, const struct hal_iwdg_config* cfg)
{
    (void)pdev;
    (void)cfg;
    return MINI_ERR_NOTSUPP;
}

int hal_iwdg_start(struct hal_iwdg_dev* pdev)
{
    (void)pdev;
    return MINI_ERR_NOTSUPP;
}

int hal_iwdg_feed(struct hal_iwdg_dev* pdev)
{
    (void)pdev;
    return MINI_ERR_NOTSUPP;
}

int hal_iwdg_set_timeout_ms(struct hal_iwdg_dev* pdev, uint32_t timeout_ms)
{
    (void)pdev;
    (void)timeout_ms;
    return MINI_ERR_NOTSUPP;
}

int hal_iwdg_set_long_timeout(struct hal_iwdg_dev* pdev)
{
    (void)pdev;
    return MINI_ERR_NOTSUPP;
}

int hal_iwdg_restore_timeout(struct hal_iwdg_dev* pdev)
{
    (void)pdev;
    return MINI_ERR_NOTSUPP;
}
