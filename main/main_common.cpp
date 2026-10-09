/**
 * @file main_common.cpp
 * @author H-000-H
 * @brief boot(app image_2) 与 app(image_1) 两个入口共用的系统级实现
 * @note  接口说明见 main_common.h, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "main_common.h"
#include <cstdint>

#include "compiler_compat.h" // mini_pre_execution
#include "log.h"             // mini_log_register_tick

#if defined(CONFIG_OS_BARE)
#include "xtask.h" // x_scheduler_now (裸机调度器时基)
#elif defined(CONFIG_OS_MINI_OS)
#include "err.h"      // MINI_OS_OK
#include "schedule.h" // mini_os_get_tick
#elif defined(CONFIG_OS_FREERTOS)
#include "FreeRTOS.h"
#include "task.h" // xTaskGetTickCount
#endif

// 系统时钟配置：HSI 16MHz → PLL(M16/N192/P2) → SYSCLK 96MHz
// AHB /1，APB1 /4 (24MHz)，APB2 /2 (48MHz)
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    // 主稳压器 Scale1（1.2V 域，支持 96MHz 全速）
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    // HSI + LSI 振荡器，PLL 由 HSI 驱动
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.LSIState            = RCC_LSI_ON;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM            = 16;
    RCC_OscInitStruct.PLL.PLLN            = 192;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ            = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    // CPU/AHB/APB 总线时钟：SYSCLK = PLL 96MHz
    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                                       RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
    {
        Error_Handler();
    }

    // SystemInit 只给 HSI 默认值，此处按实际配置更新（原 system_stm32f4xx.c 职责）
    SystemCoreClock = 96000000U; // HSI 16M / M16 * N192 / P2 = 96MHz
}

// 错误处理（HAL 返回非 OK 时进入，关中断死循环）
void Error_Handler(void)
{
    __disable_irq();
    for (;;)
    {
    }
}

#ifdef USE_FULL_ASSERT
// 断言失败上报（stm32_assert.h 在 USE_FULL_ASSERT 时调用）
void assert_failed(std::uint8_t* file, std::uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif // USE_FULL_ASSERT

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
// 日志时基桥接

extern "C" int mini_log_tick_from_scheduler(void)
{
#if defined(CONFIG_OS_MINI_OS)
    mini_os_tick_t tick = 0;
    if (mini_os_get_tick(&tick) != MINI_OS_OK)
    {
        return 0;
    }
    return static_cast<int>(tick);
#elif defined(CONFIG_OS_FREERTOS)
    return static_cast<int>(xTaskGetTickCount());
#else
    return static_cast<int>(x_scheduler_now());
#endif
}

/** @brief 尽早把调度器时基接给 mini-log (构造函数, 早于 main 与任何日志输出) */
mini_pre_execution(MINI_PRE_EXEC_PRIO_RES_POOL) static void mini_log_tick_bind(void)
{
    mini_log_register_tick(mini_log_tick_from_scheduler);
}
