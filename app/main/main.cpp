/*  SPDX-License-Identifier: Apache-2.0 */
/*
 *  main.cpp — STM32F103C8T6 节点入口
 *  裸机 (CONFIG_OSAL_NULL) + xtask 协程 + eventbus
*/
#include "main.hpp"
#include "compiler_compat.h"
#include "led.hpp"
#include "osal.h"
#include "osal_null.h"
#include "xtask.h"
#include "system_init.h"
#include "system_init.hpp"
#include "driver.h"

/* LL 库头 (时钟配置用) */
#include "stm32f1xx.h"
#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_rcc.h"
#include "stm32f1xx_ll_system.h"

void SystemClock_Config(void);

extern"C" int stm32f103c8t6_node(void)
{
    HAL_Init();
    SystemClock_Config();
    mini_tree_pre_os_init();
    board_register_all_drivers();
    mini_tree_start_tasks();

    COMPAT_IGNORE_RESULT(osal_task_create("led_task", Led::Led_Task::Led_Period, 8,Led::led_task_entry, nullptr));

    system_init_complete();
    xscheduler_start(); 
    while (1)
    {
        mini_tree_system_loop(); 
    }
    return 0;
}

void SystemClock_Config(void)
{
    /* LL 官方写法 (对照 stm32_slice): HSE + PLL×9 → SYSCLK 72MHz
     * AHB=DIV1, APB1=DIV2, APB2=DIV1; 无 RTC, 无 HSI 使能 */
    uint32_t timeout = 0xFFFFU;

    /* 1. FLASH 预取 + 等待周期 (72MHz 需 2 WS) */
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_2);

    /* 2. HSE 使能并等待就绪 */
    LL_RCC_HSE_Enable();
    while (!LL_RCC_HSE_IsReady())
    {
        if (--timeout == 0U) return;
    }

    /* 3. PLL: 源=HSE/1, 倍频=×9 → 72MHz */
    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE_DIV_1, LL_RCC_PLL_MUL_9);
    LL_RCC_PLL_Enable();
    timeout = 0xFFFFU;
    while (!LL_RCC_PLL_IsReady())
    {
        if (--timeout == 0U) return;
    }

    /* 4. 总线分频: AHB=1, APB1=/2, APB2=/1 */
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_2);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);

    /* 5. 切换到 PLL 作为系统时钟 */
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    timeout = 0xFFFFU;
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL)
    {
        if (--timeout == 0U) return;
    }

    /* 6. 更新全局时钟频率变量 (LL_USART_Init 等用它算外设时钟) */
    SystemCoreClock = 72000000U;
}
