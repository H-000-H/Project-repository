/**
 * @file main_boot.cpp
 * @author H-000-H
 * @brief STM32F407ZGT6 bootloader 入口 (0x08000000)
 * @details 只做引导仲裁: HAL_Init → flash/状态后端 → state_load(pending 回滚) → 按 current 跳转。
 *          不下载、不跑业务; 下载与激活由 app 侧 Ota 任务负责。
 * @note  旧名 main_iamge2.cpp: 与槽位号(image_1/image_2)撞名却指的不是一回事
 *        (本文件是 boot, 在 0x08000000; 槽位是 0x08020000 / 0x08080000), 故改名。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "main_common.h"   /* SystemClock_Config / Error_Handler (与 app 共用) */
#include "boot_redef.h"
#include "boot.h"          /* mini_boot_app_area_t / boot_jump_switch_app */
#include "start.h"         /* mini_boot_state_load / ota_current_partition_get */
#include "flash.h"         /* FLASH_AREA_ID_* */
#include "ota_state.h"     /* OTA_STATE_PARTITION_IMAGE_1 */
#include "flash_stm32f4.h"
#include <cstdint>

extern "C" int stm32f407zgt6_node_main(void)
{
    HAL_Init();
    flash_stm32f4_init();        /* flash + 状态后端(已含) */
    mini_boot_state_load();      /* pending 未确认 → 回滚旧分区并记失败码 */
    SystemClock_Config();

    std::uint32_t cur        = ota_current_partition_get();
    mini_boot_app_area_t app = (cur == OTA_STATE_PARTITION_IMAGE_1)
                                   ? (mini_boot_app_area_t){ 0x08080000, 0x00060000, FLASH_AREA_ID_IMAGE_1, 0 }
                                   : (mini_boot_app_area_t){ 0x08020000, 0x00060000, FLASH_AREA_ID_IMAGE_0, 0 };
    boot_jump_switch_app(app); // 校验向量表后跳转, 不返回
    for (;;)
    {
    }
}
