/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file        hal_iwdg.c
 * @brief       STM32F4 IWDG HAL 实现 (KR/PR/RLR, LSI≈32kHz)
 */
#include "hal_iwdg.h"
#include "compiler_compat.h"
#include "stm32f4xx.h"

/* IWDG 密钥: 写保护解锁 / 喂狗 / 启动 */
#define IWDG_KR_KEY_RELOAD   0xAAAAU
#define IWDG_KR_KEY_ENABLE   0xCCCCU
#define IWDG_KR_KEY_WRITE    0x5555U

/** IWDG 硬件最大超时 (ms): PR=6, RLR=0xFFF, LSI≈32kHz → (4096*256)/32 */
#define IWDG_TIMEOUT_MS_MAX  32768U

/**
 * @brief 按 timeout_ms 选最优 PR/RLR (误差最小); 超范围时落到硬件上限档
 * @param timeout_ms 目标超时 (ms)
 * @param prer 输出预分频 (PR, 0..6)
 * @param rlr 输出重载值 (RLR, 0..0xFFF)
 * @note  timeout ≈ (rlr+1) * (4<<pr) / 32 ms
 */
static void iwdg_compute(uint32_t timeout_ms, uint32_t* prer, uint32_t* rlr)
{
    /* 初始值取硬件上限, 保证超大 timeout 仍有合法档位 */
    uint32_t best_pr = 6, best_rl = 0xFFF, best_err = 0xFFFFFFFFU;
    uint32_t pr;
    for (pr = 0; pr <= 6; pr++)
    {
        uint32_t presc = 4U << pr;
        uint32_t ticks = (timeout_ms * 32U) / presc; /* ms * 32k / 1000 / presc */
        if (ticks == 0)
            ticks = 1;
        if (ticks > 0x1000U)
            continue;
        {
            uint32_t actual = (ticks * presc) / 32U;
            uint32_t err = (actual > timeout_ms) ? (actual - timeout_ms) : (timeout_ms - actual);
            if (err < best_err)
            {
                best_err = err;
                best_pr = pr;
                best_rl = ticks - 1U;
            }
        }
    }
    *prer = best_pr;
    *rlr = best_rl;
}

/**
 * @brief 初始化 IWDG 设备 (保存配置, 可选自动计算 PR/RLR)
 * @param pdev IWDG 设备指针
 * @param cfg IWDG 配置 (timeout_ms 必填)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_iwdg_init(struct hal_iwdg_dev* pdev, const struct hal_iwdg_config* cfg)
{
    if (!pdev || !cfg || cfg->timeout_ms == 0)
        return MINI_ERR_INVAL;
    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->cfg = *cfg;
    pdev->normal_timeout_ms = cfg->timeout_ms;
    if (cfg->prer == 0xFFFFFFFFU || cfg->rlr == 0xFFFFFFFFU)
        iwdg_compute(cfg->timeout_ms, &pdev->cfg.prer, &pdev->cfg.rlr);
    return MINI_OK;
}

/**
 * @brief 启动 IWDG (写 PR/RLR 并 enable)
 * @param pdev IWDG 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_iwdg_start(struct hal_iwdg_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;
    IWDG->KR = IWDG_KR_KEY_WRITE;
    IWDG->PR = pdev->cfg.prer & 7U;
    IWDG->RLR = pdev->cfg.rlr & 0xFFFU;
    IWDG->KR = IWDG_KR_KEY_RELOAD;
    IWDG->KR = IWDG_KR_KEY_ENABLE;
    pdev->active = 1;
    return MINI_OK;
}

/**
 * @brief 喂 IWDG (写 KR reload)
 * @param pdev IWDG 设备指针
 * @return 成功返回 MINI_OK, 未启动返回 MINI_ERR_NODEV
 */
int hal_iwdg_feed(struct hal_iwdg_dev* pdev)
{
    if (!pdev || !pdev->active)
        return MINI_ERR_NODEV;
    IWDG->KR = IWDG_KR_KEY_RELOAD;
    return MINI_OK;
}

/**
 * @brief 设置 IWDG 超时 (重算 PR/RLR, 已启动时热更新)
 * @param pdev IWDG 设备指针
 * @param timeout_ms 目标超时 (ms)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_iwdg_set_timeout_ms(struct hal_iwdg_dev* pdev, uint32_t timeout_ms)
{
    if (!pdev || timeout_ms == 0)
        return MINI_ERR_INVAL;
    pdev->cfg.timeout_ms = timeout_ms;
    iwdg_compute(timeout_ms, &pdev->cfg.prer, &pdev->cfg.rlr);
    /* 已启动: 写 KR=0x5555 解锁后立即更新 PR/RLR 并 reload (热更新) */
    if (pdev->active)
    {
        IWDG->KR = IWDG_KR_KEY_WRITE;
        IWDG->PR = pdev->cfg.prer & 7U;
        IWDG->RLR = pdev->cfg.rlr & 0xFFFU;
        IWDG->KR = IWDG_KR_KEY_RELOAD;
    }
    return MINI_OK;
}

/**
 * @brief 切换到 IWDG 硬件最大超时档 (~32s)
 * @param pdev IWDG 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_iwdg_set_long_timeout(struct hal_iwdg_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;
    return hal_iwdg_set_timeout_ms(pdev, IWDG_TIMEOUT_MS_MAX);
}

/**
 * @brief 恢复 IWDG 为 init 时保存的正常超时
 * @param pdev IWDG 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_iwdg_restore_timeout(struct hal_iwdg_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;
    return hal_iwdg_set_timeout_ms(pdev, pdev->normal_timeout_ms);
}
