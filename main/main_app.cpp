/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file main_app.cpp
 * @brief STM32F407ZGT6 应用入口 (基址由链接脚本决定; 当前无 bootloader, 从 0x08000000 起)
 * @author H-000-H
 * @details 点火流程：VTOR → HAL_Init → SystemClock_Config → mini_tree 两段式
 *          OTA 的固件长度与启动由业务侧(命令/协议)按需调用, 此处只注册任务:
 *              app::Ota::GetInstance().RequestOta(fw_len);
 * @note  启动尾段按 OS 后端分支 (见 system_init.h 的启动时序):
 *          裸机: xscheduler_start → system_init_complete → super-loop
 *          OS  : system_init_complete → mini_scheduler_start(本人除非小资源不然不喜欢用裸机)
 *          main没有任何逻辑只有注册和ota 逻辑和实现在不同目录下面
 */      
#include "main.h"

#include "app_uart_cmd.hpp"
#include "app_uart_recv.hpp"
#include "driver.h"
#include "flash_stm32f4.h" /* flash_stm32f4_init: 注册 flash ops + OTA 状态后端 */
#include "app_button.hpp"
#include "app_led.hpp"
#include "app_ota.hpp"
#include "start.h" /* mini_boot_state_refresh / mini_boot_confirm_ota */
#include "system_init.h"
#include "system_log.h"
#include "boot_redef.h"
#include "app_ui.hpp"
#if defined(CONFIG_OS_BARE)
#include "xtask.h" /* 仅裸机后端存在 xtask 调度器接口 */
#endif
#if !defined(CONFIG_OS_BARE)
#include "mini_backend.h"
#endif
#include "main_common.h"

/**
 * @brief 本固件所在分区的基址
 * @note  由链接脚本导出: PROVIDE(__app_partition_base = ORIGIN(FLASH));
 *        本固件从该基址开始运行(复位后拿它设 VTOR), 要挪基址/恢复 OTA 只改链接脚本即可
 */
extern "C" const uint32_t __app_partition_base;

/**
 * @brief 应用入口
 */
extern "C" __attribute__((used)) int stm32f407zgt6_node_main(void)
{
    /* 重定位向量表到本固件所在分区 */
    mini_boot_set_vtor((uint32_t)&__app_partition_base);

    HAL_Init();

    /* 系统时钟：HSI + PLL → 96MHz */
    SystemClock_Config();

    /* ===== 暂时关闭 OTA (先保证 app 能正常跑) =====
     * 原因: app 用 image1.ld 从 0x08000000 起占满整片 1MB */
    // flash_stm32f4_init();
    // const bool ota_state_ok = (mini_boot_state_refresh() == ERR_OK);

    mini_tree_pre_os_init();

    // if (!ota_state_ok)
    // {
    //     MT_LOG_ERROR("Ota", "state refresh failed: flash backend not ready");
    // }

    board_register_all_drivers();

    mini_tree_start_tasks();

    if (!app::Led::GetInstance().ThreadRegister())
    {
        MT_LOG_ERROR("App", "led task register failed");
    }
    if (!app::Button::ThreadRegister())
    {
        MT_LOG_ERROR("App", "button task register failed");
    }
    /* 恢复 OTA 时把这段打开 */
    // if (!app::Ota::GetInstance().ThreadRegister())
    // {
    //     MT_LOG_ERROR("App", "ota task register failed");
    // }
    if (!app::UartCommunicate::GetInstance().ThreadRegister())
    {
        MT_LOG_ERROR("App", "uart task register failed");
    }

    static app::Ui s_ui("st7789", 10U);
    if (!s_ui.ThreadRegister())
    {
        MT_LOG_ERROR("App", "ui task register failed");
    }
    /* 命令层上线: 注册 led.set + 把收包回调挂到 UART 实例上 */
    app::Cmd::Init();


    system_init_complete();

    /* 确认本分区镜像(清 pending + trial)：boot 已放行过本次试运行(trial=1)，确认后即"转正"；
     * 不确认则下次复位被 boot 判为试运行超时 → 回滚到旧分区。*/
    // if (mini_boot_confirm_ota() != ERR_OK)
    // {
    //     MT_LOG_ERROR("Ota", "confirm failed: state backend not ready");
    // }

    (void)mini_scheduler_start();
    for (;;)
    {
    }
}
