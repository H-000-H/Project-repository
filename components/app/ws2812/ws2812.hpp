/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 只走 device / VFS / OSAL；返回值用 etl::optional。
 */
#pragma once

#include "etl/optional.h"

/**
 * @brief 打开 ws2812 设备并注册 6 色闪烁任务
 * @return 成功返回 0；失败返回 etl::nullopt
 */
etl::optional<int> app_ws2812_task_start();
