/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-C3 安全停机垫底 — 全屏蔽策略下 (mini_tree ESP 构建不编 weak stub) 的
 * strong 占位。safe_state.c 的 enter_safe_state 会调用:
 *   hal_platform_critical_hardware_lock — 返回 MINI_OK 视为无硬件锁需求 (ESP 靠
 *     portENTER_CRITICAL / 关中断, 不依赖外设级锁);
 *   hal_platform_nmi_emergency_stamp    — 空实现 (NMI 应急标记仅裸机调试用);
 *   hal_pwm_force_stop_all              — 空实现 (安全停机时强制停 PWM 输出;
 *     ESP 需用时在此调用 ledc/rmt 全停)。
 */
#include "status.h"

int hal_platform_critical_hardware_lock(void) { return MINI_OK; }

int hal_pwm_force_stop_all(void) { return MINI_OK; }

void hal_platform_nmi_emergency_stamp(void) {}
