/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file        hal_rtc.c
 * @brief       STM32F4 RTC HAL 实现 (日历/闹钟寄存器; alarm ISR 派发待补)
 */
#include "hal_rtc.h"
#include "compiler_compat.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_rtc.h"
#include "stm32f4xx_ll_rcc.h"
#include "stm32f4xx_ll_pwr.h"
#include "stm32f4xx_ll_bus.h"

/**
 * @brief 进入 RTC 初始化模式并等待 INIT 标志
 * @param rtc RTC 外设指针
 * @return 成功返回 MINI_OK, 超时返回 MINI_ERR_TIMEOUT
 */
static int rtc_enter_init_mode(RTC_TypeDef* rtc)
{
    uint32_t guard = 0x100000U;
    if (LL_RTC_IsActiveFlag_INIT(rtc))
        return MINI_OK;
    LL_RTC_EnableInitMode(rtc);
    while (!LL_RTC_IsActiveFlag_INIT(rtc))
    {
        if (--guard == 0)
            return MINI_ERR_TIMEOUT;
    }
    return MINI_OK;
}

/**
 * @brief 退出 RTC 初始化模式
 * @param rtc RTC 外设指针
 */
static void rtc_exit_init_mode(RTC_TypeDef* rtc)
{
    LL_RTC_DisableInitMode(rtc);
}

/**
 * @brief 初始化 RTC 设备对象 (保存配置)
 * @param pdev RTC 设备指针
 * @param cfg RTC 配置
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_init(struct hal_rtc_dev* pdev, const struct hal_rtc_config* cfg)
{
    if (!pdev || !cfg || !cfg->rtc)
        return MINI_ERR_INVAL;
    MINI_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->cfg = *cfg;
    return MINI_OK;
}

/**
 * @brief 释放 RTC 设备 (若已打开则 close)
 * @param pdev RTC 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_rtc_deinit(struct hal_rtc_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;
    if (pdev->hw_open)
        (void)hal_rtc_close(pdev);
    MINI_MEM_SET(pdev, 0, sizeof(*pdev));
    return MINI_OK;
}

/**
 * @brief 打开 RTC (LSI/RTC 时钟、预分频、24h 格式)
 * @param pdev RTC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_TIMEOUT
 */
int hal_rtc_open(struct hal_rtc_dev* pdev)
{
    RTC_TypeDef* rtc;
    int ret;

    if (!pdev || !pdev->cfg.rtc)
        return MINI_ERR_INVAL;
    if (pdev->hw_open)
        return MINI_OK;

    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
    LL_PWR_EnableBkUpAccess();

    if (pdev->cfg.clk_source == 0)
    {
        LL_RCC_LSI_Enable();
        while (!LL_RCC_LSI_IsReady())
        {
        }
        LL_RCC_SetRTCClockSource(LL_RCC_RTC_CLKSOURCE_LSI);
    }
    LL_RCC_EnableRTC();

    LL_RTC_DisableWriteProtection(rtc);
    ret = rtc_enter_init_mode(rtc);
    if (ret != MINI_OK)
    {
        LL_RTC_EnableWriteProtection(rtc);
        return ret;
    }

    LL_RTC_SetAsynchPrescaler(rtc, pdev->cfg.async_prediv ? pdev->cfg.async_prediv : 127U);
    LL_RTC_SetSynchPrescaler(rtc, pdev->cfg.sync_prediv ? pdev->cfg.sync_prediv : 255U);
    if (pdev->cfg.format_24h)
        LL_RTC_SetHourFormat(rtc, LL_RTC_HOURFORMAT_24HOUR);
    else
        LL_RTC_SetHourFormat(rtc, LL_RTC_HOURFORMAT_AMPM);

    rtc_exit_init_mode(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    pdev->hw_open = 1;
    return MINI_OK;
}

/**
 * @brief 关闭 RTC (清 hw_open 与 alarm 回调)
 * @param pdev RTC 设备指针
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_rtc_close(struct hal_rtc_dev* pdev)
{
    if (!pdev)
        return MINI_ERR_INVAL;
    pdev->hw_open = 0;
    pdev->alarm_cb = NULL;
    pdev->alarm_user = NULL;
    return MINI_OK;
}

/**
 * @brief 设置 RTC 日历时间 (TIME + DATE)
 * @param pdev RTC 设备指针
 * @param time 时间结构 (二进制字段)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_TIMEOUT
 */
int hal_rtc_set_time(struct hal_rtc_dev* pdev, const struct hal_rtc_time* time)
{
    RTC_TypeDef* rtc;
    int ret;

    if (!pdev || !time || !pdev->hw_open)
        return MINI_ERR_INVAL;
    if (time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31)
        return MINI_ERR_INVAL;

    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    LL_RTC_DisableWriteProtection(rtc);
    ret = rtc_enter_init_mode(rtc);
    if (ret != MINI_OK)
    {
        LL_RTC_EnableWriteProtection(rtc);
        return ret;
    }

    LL_RTC_TIME_Config(rtc, LL_RTC_TIME_FORMAT_AM_OR_24,
                       __LL_RTC_CONVERT_BIN2BCD(time->hour),
                       __LL_RTC_CONVERT_BIN2BCD(time->minute),
                       __LL_RTC_CONVERT_BIN2BCD(time->second));
    LL_RTC_DATE_Config(rtc,
                       time->weekday ? time->weekday : LL_RTC_WEEKDAY_MONDAY,
                       __LL_RTC_CONVERT_BIN2BCD(time->day),
                       __LL_RTC_CONVERT_BIN2BCD(time->month),
                       __LL_RTC_CONVERT_BIN2BCD((uint8_t)(time->year % 100U)));

    rtc_exit_init_mode(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    return MINI_OK;
}

/**
 * @brief 读取 RTC 日历时间
 * @param pdev RTC 设备指针
 * @param time 输出时间结构
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_get_time(struct hal_rtc_dev* pdev, struct hal_rtc_time* time)
{
    RTC_TypeDef* rtc;
    uint32_t t, d;

    if (!pdev || !time || !pdev->hw_open)
        return MINI_ERR_INVAL;

    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    t = LL_RTC_TIME_Get(rtc);
    d = LL_RTC_DATE_Get(rtc);

    time->hour    = (uint8_t)__LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_HOUR(t));
    time->minute  = (uint8_t)__LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_MINUTE(t));
    time->second  = (uint8_t)__LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_SECOND(t));
    time->weekday = (uint8_t)__LL_RTC_GET_WEEKDAY(d);
    time->day     = (uint8_t)__LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_DAY(d));
    time->month   = (uint8_t)__LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_MONTH(d));
    time->year    = (uint16_t)(2000U + __LL_RTC_CONVERT_BCD2BIN(__LL_RTC_GET_YEAR(d)));
    return MINI_OK;
}

/**
 * @brief 设置 RTC 闹钟 A 并注册回调
 * @param pdev RTC 设备指针
 * @param alarm 闹钟时间
 * @param cb 闹钟回调
 * @param user 用户数据指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_set_alarm(struct hal_rtc_dev* pdev, const struct hal_rtc_time* alarm,
                  hal_rtc_alarm_cb_t cb, void* user)
{
    RTC_TypeDef* rtc;

    if (!pdev || !alarm || !pdev->hw_open)
        return MINI_ERR_INVAL;

    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    pdev->alarm_cb = cb;
    pdev->alarm_user = user;

    LL_RTC_DisableWriteProtection(rtc);
    LL_RTC_ALMA_Disable(rtc);
    LL_RTC_ClearFlag_ALRA(rtc);
    LL_RTC_ALMA_ConfigTime(rtc, LL_RTC_ALMA_TIME_FORMAT_AM,
                           __LL_RTC_CONVERT_BIN2BCD(alarm->hour),
                           __LL_RTC_CONVERT_BIN2BCD(alarm->minute),
                           __LL_RTC_CONVERT_BIN2BCD(alarm->second));
    LL_RTC_ALMA_SetDay(rtc, __LL_RTC_CONVERT_BIN2BCD(alarm->day));
    LL_RTC_ALMA_SetMask(rtc, LL_RTC_ALMA_MASK_NONE);
    LL_RTC_ALMA_Enable(rtc);
    LL_RTC_EnableIT_ALRA(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    return MINI_OK;
}

/**
 * @brief 取消 RTC 闹钟 A
 * @param pdev RTC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_cancel_alarm(struct hal_rtc_dev* pdev)
{
    RTC_TypeDef* rtc;

    if (!pdev || !pdev->hw_open)
        return MINI_ERR_INVAL;
    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    LL_RTC_DisableWriteProtection(rtc);
    LL_RTC_ALMA_Disable(rtc);
    LL_RTC_DisableIT_ALRA(rtc);
    LL_RTC_ClearFlag_ALRA(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    pdev->alarm_cb = NULL;
    pdev->alarm_user = NULL;
    return MINI_OK;
}

/**
 * @brief 设置 RTC 唤醒定时器 (秒级, CKSPRE)
 * @param pdev RTC 设备指针
 * @param seconds 唤醒间隔 (秒, >0)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_set_wakeup_timer(struct hal_rtc_dev* pdev, uint32_t seconds)
{
    RTC_TypeDef* rtc;

    if (!pdev || !pdev->hw_open || seconds == 0)
        return MINI_ERR_INVAL;
    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    LL_RTC_DisableWriteProtection(rtc);
    LL_RTC_WAKEUP_Disable(rtc);
    while (!LL_RTC_IsActiveFlag_WUTW(rtc))
    {
    }
    LL_RTC_WAKEUP_SetClock(rtc, LL_RTC_WAKEUPCLOCK_CKSPRE);
    LL_RTC_WAKEUP_SetAutoReload(rtc, seconds - 1U);
    LL_RTC_ClearFlag_WUT(rtc);
    LL_RTC_EnableIT_WUT(rtc);
    LL_RTC_WAKEUP_Enable(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    return MINI_OK;
}

/**
 * @brief 取消 RTC 唤醒定时器
 * @param pdev RTC 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_rtc_cancel_wakeup_timer(struct hal_rtc_dev* pdev)
{
    RTC_TypeDef* rtc;

    if (!pdev || !pdev->hw_open)
        return MINI_ERR_INVAL;
    rtc = (RTC_TypeDef*)pdev->cfg.rtc;
    LL_RTC_DisableWriteProtection(rtc);
    LL_RTC_WAKEUP_Disable(rtc);
    LL_RTC_DisableIT_WUT(rtc);
    LL_RTC_ClearFlag_WUT(rtc);
    LL_RTC_EnableWriteProtection(rtc);
    return MINI_OK;
}

/**
 * @brief 强制停止 RTC 闹钟与唤醒 (全局 RTC 寄存器)
 */
void hal_rtc_force_stop(void)
{
    LL_RTC_DisableWriteProtection(RTC);
    LL_RTC_ALMA_Disable(RTC);
    LL_RTC_WAKEUP_Disable(RTC);
    LL_RTC_EnableWriteProtection(RTC);
}
