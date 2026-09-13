/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file        hal_wwdg.c
 * @brief       STM32F4 WWDG HAL 实现 (CFR/CR, APB1)
 */
#include "hal_wwdg.h"
#include "compiler_compat.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"

/**
 * @brief 初始化 WWDG 设备 (校验 counter/window)
 * @param pdev WWDG 设备指针
 * @param cfg WWDG 配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_wwdg_init(struct hal_wwdg_dev* pdev, const struct hal_wwdg_config* cfg)
{
    if (!pdev || !cfg)
        return MINI_ERR_INVAL;
    if (cfg->counter < 0x40U || cfg->counter > 0x7FU)
        return MINI_ERR_INVAL;
    if (cfg->window > 0x7FU)
        return MINI_ERR_INVAL;
    MINI_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->cfg = *cfg;
    return MINI_OK;
}

/**
 * @brief 启动 WWDG (写 CFR/CR, 使能 WDGA)
 * @param pdev WWDG 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_wwdg_start(struct hal_wwdg_dev* pdev)
{
    uint32_t cfr;

    if (!pdev)
        return MINI_ERR_INVAL;

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_WWDG);

    cfr = (pdev->cfg.prescaler & 3U) << 7;
    cfr |= (pdev->cfg.window & 0x7FU);
    if (pdev->cfg.ewi_enable)
        cfr |= (1U << 9);
    WWDG->CFR = cfr;
    WWDG->CR = (1U << 7) | (pdev->cfg.counter & 0x7FU); /* WDGA | T[6:0] */
    pdev->active = 1;
    return MINI_OK;
}

/**
 * @brief 喂 WWDG (窗口内重写 CR)
 * @param pdev WWDG 设备指针
 * @return 成功返回 MINI_OK, 未启动返回 MINI_ERR_NODEV
 */
int hal_wwdg_feed(struct hal_wwdg_dev* pdev)
{
    if (!pdev || !pdev->active)
        return MINI_ERR_NODEV;
    /* 必须在窗口内写 T; 上层负责时机 */
    WWDG->CR = (1U << 7) | (pdev->cfg.counter & 0x7FU);
    return MINI_OK;
}
