/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file main.cpp
 *@brief CH32V307 应用入口 (单文件骨架)
 *@author H-000-H
 *@details
 *   启动流程: startup_ch32v30x_D8C.S -> SystemInit -> mret -> ch307_node_main。
 *   OSAL 后端: 裸机 (CONFIG_OSAL_NULL) + xtask 抢占式时间片 (CONFIG_XTASK_PREEMPT),
 *   与 mini_tree/.config 保持一致; 业务任务统一走 osal_task_create。
 *   此处只做 mini_tree 两段式点火并进入 super-loop, 不返回。
 */
#include "system_init.h"
#include "driver.h"
#include "compiler_compat.h"

#include "xtask.h"

/**
 * @brief 应用入口 (由 startup 经 mret 跳入, 不返回)
 * @return 正常不返回
 */
extern "C" __attribute__((used)) int ch307_node_main(void)
{
    /* mini_tree 两段式点火 (时钟由 startup SystemInit 完成) */
    mini_tree_pre_os_init();
    board_register_all_drivers();
    mini_tree_start_tasks();

    /* xtask 抢占式调度器启动 (绑定 SysTick), 随后释放全局中断 */
    xscheduler_start();
    system_init_complete();

    /* 裸机 super-loop: 轮询框架任务与时间片调度 */
    for (;;)
    {
        mini_tree_system_loop();
    }
}
