/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file main_iamge2.cpp
 *@brief STM32F407ZGT6 bootloader 入口 (image_2 引导态)
 *@author H-000-H
 *@details 只做引导仲裁: HAL_Init → flash/状态后端 → state_load(pending 回滚) → 按 current 跳转。
 *          不下载、不跑业务; 下载与激活由 app 侧 Ota 任务负责。
 */
#include "main_common.h"   /* SystemClock_Config / Error_Handler (与 app 共用) */
#include "boot_redef.h"
#include "boot.h"          /* mini_boot_app_area_t / boot_jump_switch_app */
#include "start.h"         /* mini_boot_state_load / ota_current_partition_get */
#include "flash.h"         /* FLASH_AREA_ID_* */
#include "ota_state.h"     /* OTA_STATE_PARTITION_IMAGE_1 */
#include "flash_stm32f4.h"

extern "C" int stm32f407zgt6_node_main(void)
{
    HAL_Init();
    flash_stm32f4_init();        /* flash + 状态后端(已含) */
    mini_boot_state_load();      /* pending 未确认 → 回滚旧分区并记失败码 */
    SystemClock_Config();

    uint32_t cur = ota_current_partition_get();
    mini_boot_app_area_t app = (cur == OTA_STATE_PARTITION_IMAGE_1)
        ? (mini_boot_app_area_t){ 0x08080000, 0x00060000, FLASH_AREA_ID_IMAGE_1, 0 }
        : (mini_boot_app_area_t){ 0x08020000, 0x00060000, FLASH_AREA_ID_IMAGE_0, 0 };
    boot_jump_switch_app(app);   /* 校验向量表后跳转, 不返回 */
    for (;;) {}
}
