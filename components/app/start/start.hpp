/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 应用入口 — 按 app_pre_must_view 规则：返回 etl::optional，无异常/堆
 */
#pragma once

#include "etl/optional.h"

/**
 * @brief 应用启动：mini_tree 初始化 + 注册业务任务
 * @return 成功返回 0；失败返回 etl::nullopt
 */
etl::optional<int> start();
