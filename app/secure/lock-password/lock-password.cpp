/**
 * @file lock-password.cpp
 * @author H-000-H
 * @brief  锁屏密码实现
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "lock-password.hpp"
#include "system_log.h"
#include <cstdint>
namespace app
{
    constexpr const char*const kTag = "lock-pwd";

    /**
     * @brief  设/改密码: 校验非空且不超长后写入
     * @param  text 新密码
     * @return 成功返回 true; 空或超长返回 false
     */
    bool LockPassword::Set(const etl::string_view& text)
    {
        if(text.empty())
        {
            MT_LOG_ERROR(kTag,"密码不能为空");
            return false;
        }
        if(text.size() > static_cast<std::size_t>(kLockPasswordMaxLen))
        {
            MT_LOG_ERROR(kTag,"密码太长(%u > %u), 拒绝",
                         static_cast<unsigned>(text.size()),
                         static_cast<unsigned>(kLockPasswordMaxLen));
            return false;
        }
        this->pwd.assign(text.begin(), text.end());
        return true;
    }

    /**
     * @brief  校验输入是否与已设密码一致
     * @param  text 待校验的输入
     * @return 匹配返回 true; 未设密码或不匹配返回 false
     */
    bool LockPassword::Match(const etl::string_view& text) const
    {
        if(this->pwd.empty())
            return false;   /* 还没设过密码: 不放行 */
        return (etl::string_view(this->pwd) == text);
    }
}
