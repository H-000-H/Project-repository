/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file main_app.cpp
 * @brief STM32F407ZGT6 应用入口: 只做点火 + 任务注册, 业务逻辑与实现都在 app/ 各目录
 * @author H-000-H
 * @note  启动尾段按 OS 后端分支 (见 system_init.h): 裸机走 super-loop, OS 走 mini_scheduler_start
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
#include "input_backend.hpp"
#include "ui_task.hpp"
#if defined(CONFIG_OS_BARE)
#include "xtask.h" /* 仅裸机后端存在 xtask 调度器接口 */
#endif
#if !defined(CONFIG_OS_BARE)
#include "mini_backend.h"
#endif
#include "main_common.h"

/** @brief 本固件所在分区的基址 (链接脚本 PROVIDE 导出; 复位后拿它设 VTOR) */
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

    /* ===== 暂时关闭 OTA =====
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

    /* UI 输入后端: 按键。必须在按键任务之前备好 —— 它的两个钩子要在按键线程起来前挂上 */
    static ui::KeypadInput s_input("button2");
    app::Button::set_sample_hook(&ui::KeypadInput::open_thunk, &ui::KeypadInput::sample_thunk, &s_input);

    if (!app::Led::get_instance().thread_register())
    {
        MT_LOG_ERROR("App", "led task register failed");
    }
    if (!app::Button::thread_register())
    {
        MT_LOG_ERROR("App", "button task register failed");
    }
    /* 恢复 OTA 时把这段打开 */
    // if (!app::Ota::get_instance().thread_register())
    // {
    //     MT_LOG_ERROR("App", "ota task register failed");
    // }
    if (!app::UartCommunicate::get_instance().thread_register())
    {
        MT_LOG_ERROR("App", "uart task register failed");
    }

    static app::UiTask s_ui("st7789", 10U);
    if (!s_ui.thread_register(s_input))
    {
        MT_LOG_ERROR("App", "ui task register failed");
    }
    /* 命令层上线: 注册 led.set + 把收包回调挂到 UART 实例上 */
    app::Cmd::init();

    system_init_complete();

    /* 确认本分区镜像(清 pending + trial): boot 已放行过本次试运行, 确认后即"转正";
     * 不确认则下次复位被判试运行超时 → 回滚到旧分区 */
    // if (mini_boot_confirm_ota() != ERR_OK)
    // {
    //     MT_LOG_ERROR("Ota", "confirm failed: state backend not ready");
    // }

    (void)mini_scheduler_start();
    for (;;)
    {
    }
}
