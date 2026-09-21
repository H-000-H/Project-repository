/**
 * @file lock-password.cpp
 * @author H-000-H
 * @brief  锁屏密码实现
 * @note   接口说明见 lock-password.hpp。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "lock-password.hpp"
#include "system_log.h"
#include <cstdint>
namespace app
{
    constexpr const char* const k_tag = "lock-pwd";

    bool LockPassword::set(const etl::string_view& text)
    {
        if (text.empty())
        {
            MT_LOG_ERROR(k_tag, "密码不能为空");
            return false;
        }
        if (text.size() > static_cast<std::size_t>(k_lock_password_max_len))
        {
            MT_LOG_ERROR(k_tag, "密码太长(%u > %u), 拒绝",
                         static_cast<unsigned>(text.size()),
                         static_cast<unsigned>(k_lock_password_max_len));
            return false;
        }
        this->pwd.assign(text.begin(), text.end());
        return true;
    }

    bool LockPassword::match(const etl::string_view& text) const
    {
        if (this->pwd.empty())
            return false; /* 还没设过密码: 不放行 */
        return (etl::string_view(this->pwd) == text);
    }
}
