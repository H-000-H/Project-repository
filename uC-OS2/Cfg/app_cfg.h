/* SPDX-License-Identifier: Apache-2.0 */
/*
 * app_cfg.h — mini_tree 项目 uC/OS-II 应用配置 (项目自有副本)
 * ARMv7-M 端口 os_cpu.h 强制要求以下两个宏 (模板为空壳, 此副本补齐)
 */
#ifndef APP_CFG_H
#define APP_CFG_H

/* 内核感知中断边界: 优先级 ≥4 的中断被 OS_CRITICAL 屏蔽 (BASEPRI 方案) */
#define CPU_CFG_KA_IPL_BOUNDARY                             4u

/* NVIC 优先级位数 (Cortex-M4F = 4 位, 16 级优先级) */
#define CPU_CFG_NVIC_PRIO_BITS                              4u

#endif /* APP_CFG_H */
