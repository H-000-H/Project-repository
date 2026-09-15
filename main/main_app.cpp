/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file main_app.cpp
 * @brief STM32F407ZGT6 应用入口 (0x08020000 或 0x08080000, 由链接脚本决定)
 * @author H-000-H
 * @details 点火流程：VTOR → HAL_Init → SystemClock_Config → mini_tree 两段式
 *          OTA 的固件长度与启动由业务侧(命令/协议)按需调用, 此处只注册任务:
 *              app_ota::Ota::GetInstance().RequestOta(fw_len);
 * @note  启动尾段按 OS 后端分支 (见 system_init.h 的启动时序):
 *          裸机: xscheduler_start → system_init_complete → super-loop
 *          OS  : system_init_complete → mini_scheduler_start(本人除非小资源不然不喜欢用裸机)
 */
#include "main.h"

#include "app_uart_cmd.hpp"
#include "app_uart_recv.hpp"
#include "driver.h"
#include "err.h"
#include "flash_stm32f4.h" /* flash_stm32f4_init: 注册 flash ops + OTA 状态后端 */
#include "app_led.hpp"
#include "app_ota.hpp"
#include "start.h" /* mini_boot_state_refresh / mini_boot_confirm_ota */
#include "system_init.h"
#include "system_log.h"
#include "boot_redef.h"

#if defined(CONFIG_OS_BARE)
#include "xtask.h" /* 仅裸机后端存在 xtask 调度器接口 */
#endif
#if !defined(CONFIG_OS_BARE)
#include "mini_backend.h" 
#endif
#include "main_common.h" 

/**
 * @brief 本固件所在分区的基址
 * @note  由链接脚本导出: PROVIDE(__app_partition_base = ORIGIN(FLASH))
 *        image_0 编译得 0x08020000, image_1 编译得 0x08080000, 无需改动代码
 */
extern "C" const uint32_t __app_partition_base;

/**
 * @brief 应用入口
 */
extern "C" __attribute__((used)) int stm32f407zgt6_node_main(void)
{
    /* 重定位向量表到本固件所在分区 */
    mini_boot_set_vtor((uint32_t)&__app_partition_base);

    /* flash 后端 + OTA 持久，要早于任何 OTA 判定：
     * - 未注册后端时 flash_area_open() 直接失败 → 下载写不进 flash
     * - 未刷新状态时 current 恒为 image_0 → 跑在 image_1 时会覆盖自己 */
    flash_stm32f4_init();
    if (mini_boot_state_refresh() != ERR_OK)
    {
        MT_LOG_ERROR("Ota", "state refresh failed: flash backend not ready");
    }

    HAL_Init();

    /* 系统时钟：HSI + PLL → 96MHz */
    SystemClock_Config();

    mini_tree_pre_os_init();

    board_register_all_drivers();

    mini_tree_start_tasks();

#if defined(CONFIG_OS_BARE)
    /* 裸机时间片调度器启动 (OS 后端由内核接管, 不走这里) */
    xscheduler_start();
#endif
    system_init_complete();

    /* 系统自检通过 → 确认本分区镜像(清 pending + trial)：
     * boot 已放行过本次试运行(trial=1)，这里确认后即"转正"；
     * 不确认则下次复位被 boot 判为试运行超时 → 回滚到旧分区，本次 OTA 白做 */
    if (mini_boot_confirm_ota() != ERR_OK)
    {
        MT_LOG_ERROR("Ota", "confirm failed: state backend not ready");
    }

    /* 业务任务注册: 必须在调度器启动之前完成。
     * OS 后端下 mini_scheduler_start() 不返回, 之后写什么都跑不到 */
    if (!app_led::Led::GetInstance().ThreadRegister())
    {
        MT_LOG_ERROR("App", "led task register failed");
    }
    if (!app_ota::Ota::GetInstance().ThreadRegister())
    {
        MT_LOG_ERROR("App", "ota task register failed");
    }
    if (!app_communicate::UartCommunicate::GetInstance().ThreadRegister())
    {
        MT_LOG_ERROR("App", "uart task register failed");
    }

    /* 命令层上线: 注册 led.set + 把收包回调挂到 UART 实例上 */
    app_cmd::Init();

#if defined(CONFIG_OS_BARE)
    /* 裸机: 调度器只做时间片轮转, 主循环由本函数持有 */
    while (1)
    {
        mini_tree_system_loop();
    }
#else
    /* OS 后端: 打开 tick 并交出控制权, 正常情况下不返回;
     * 若返回则说明内核启动失败, 停在死循环便于定位 */
    (void)mini_scheduler_start();
    for (;;)
    {
    }
#endif
}
