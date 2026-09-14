/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file main_iamge1.cpp
 *@brief STM32F407ZGT6 应用入口 (image_1 运行态)
 *@author H-000-H
 *@details 点火流程：VTOR → HAL_Init → SystemClock_Config → mini_tree 两段式
 *          OTA 的固件长度与启动由业务侧(命令/协议)按需调用, 此处只注册任务:
 *              APP_Ota::Ota::get_instance().request_ota(fw_len);
 */

#include "main.h"
#include "main_common.h"   /* SystemClock_Config / Error_Handler (与 boot 共用) */
#include "boot_redef.h"
#include "system_init.h"
#include "driver.h"
#include "xtask.h"
#include "led.hpp"
#include "ota.hpp"
#include "flash_stm32f4.h"  /* flash_stm32f4_init: 注册 flash ops + OTA 状态后端 */
#include "start.h"          /* mini_boot_state_refresh / mini_boot_confirm_ota */
#include "err.h"
#include "system_log.h"
#include "communicate_uart.hpp"
#include "cmd.hpp"          /* App_Cmd::init: 命令注册 + 收包回调挂载 */
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

    /* 系统时钟：HSI + PLL → 96MHz*/
    SystemClock_Config();

    mini_tree_pre_os_init();
    board_register_all_drivers();
    mini_tree_start_tasks();

    /* 裸机时间片调度器启动 */
    xscheduler_start();
    system_init_complete();

    /* 系统自检通过 → 确认本分区镜像(清 pending + trial)：
     * boot 已放行过本次试运行(trial=1)，这里确认后即"转正"；
     * 不确认则下次复位被 boot 判为试运行超时 → 回滚到旧分区，本次 OTA 白做 */
    if (mini_boot_confirm_ota() != ERR_OK)
    {
        MT_LOG_ERROR("Ota", "confirm failed: state backend not ready");
    }

    App_Led::Led::get_instance().thread_register();
    APP_Ota::Ota::get_instance().thread_register();
    APP_Communicate::UartCommunicate::getInstance().thread_register();
    App_Cmd::init();

    while (1)
    {
        mini_tree_system_loop();
    }
}
