/**
 *@file main.cpp
 *@brief STM32F407ZGT6 节点入口
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@author H-000-H
 *@details 点火流程：HAL_Init → SystemClock_Config → mini_tree 两段式
 */

#include "main.h"
#include "system_init.h"
#include "driver.h"
#include "xtask.h"
#include "led.hpp"

void SystemClock_Config(void);
/**
 * @brief 应用入口
 */
extern "C" __attribute__((used)) int stm32f407zgt6_node_main(void)
{
    HAL_Init();

    /* 系统时钟：HSI + PLL → 96MHz（见 SystemClock_Config） */
    SystemClock_Config();

    /* mini_tree 两段式点火：时钟已配；外设由 board.dts probe 注册（无 CubeMX MX_*） */
    mini_tree_pre_os_init();
    board_register_all_drivers();
    mini_tree_start_tasks();

    /* 裸机时间片调度器启动 */
    xscheduler_start();
    system_init_complete();
    App_Led::Led::instance().register_task();
    while (1)
    {
        mini_tree_system_loop();
    }
}

/**
 * @brief 系统时钟配置：HSI 16MHz → PLL(M16/N192/P2) → SYSCLK 96MHz
 *        AHB /1，APB1 /4 (24MHz)，APB2 /2 (48MHz)
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* 主稳压器 Scale1（1.2V 域，支持 96MHz 全速） */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* HSI + LSI 振荡器，PLL 由 HSI 驱动 */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.LSIState = RCC_LSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 16;
    RCC_OscInitStruct.PLL.PLLN = 192;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    /* CPU/AHB/APB 总线时钟：SYSCLK = PLL 96MHz */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK| RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
        Error_Handler();

    /* SystemInit 只给 HSI 默认值，此处按实际配置更新（原 system_stm32f4xx.c 职责） */
    SystemCoreClock = 96000000U; /* HSI 16M / M16 * N192 / P2 = 96MHz */
}

/**
 * @brief 错误处理（HAL 返回非 OK 时进入，关中断死循环）
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT
/**
 * @brief 断言失败上报（stm32_assert.h 在 USE_FULL_ASSERT 时调用）
 */
void assert_failed(uint8_t* file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif /* USE_FULL_ASSERT */
