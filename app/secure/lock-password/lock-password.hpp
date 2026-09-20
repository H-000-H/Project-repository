/**
 * @file lock-password.hpp
 * @author H-000-H
 * @brief  锁屏密码: 常驻数据, 跟锁屏页面(ui::Lock)没有生命周期关系 —— 页面删了它还在
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOCK_PASSWORD_HPP
#define LOCK_PASSWORD_HPP
#include "etl/atomic/atomic_gcc_sync.h"
#include "etl/string.h"
#include "etl/string_view.h"
#include <cstdint>
namespace app
{
    constexpr std::uint8_t kLockPasswordMaxLen = 16;

    class LockPassword
    {
    public:
        LockPassword() = default;  /**< 默认构造 */
        ~LockPassword() = default; /**< 默认析构 */
        LockPassword(const LockPassword&) = delete;            /**< 禁用拷贝构造 */
        LockPassword& operator=(const LockPassword&) = delete; /**< 禁用拷贝赋值 */
        LockPassword(LockPassword&&) = delete;                 /**< 禁用移动构造 */
        LockPassword& operator=(LockPassword&&) = delete;      /**< 禁用移动赋值 */
        /**
         * @brief  获取锁屏密码单例
         * @return 锁屏密码单例引用
         */
        static LockPassword& GetInstance(){static LockPassword instance ; return instance;}

        /**
         * @brief 设/改密码
         * @param text 新密码
         * @return false = 空 或 超长(超 kLockPasswordMaxLen)
         * @note  掉电不丢加flash操作
         */
        bool Set(const etl::string_view& text);
        /**
         * @brief 只比对可以直接加crc 哈希 ...
         * @param text 待校验的输入
         * @return true = 对得上
         * @note  密码为空(还没设过)时一律 false
         */
        bool Match(const etl::string_view& text) const;
        /**
         * @brief  判断是否尚未设置密码
         * @return true = 密码为空
         */
        bool Empty() const { return this->pwd.empty(); }

        /**
         * @brief  读取当前锁定状态
         * @return true = 已锁定
         */
        bool IsLocked() const { return this->is_lock.load(etl::memory_order_relaxed); }
        /**
         * @brief 设置锁定状态
         * @param locked true 锁定, false 解锁
         */
        void SetLocked(bool locked) { this->is_lock.store(locked, etl::memory_order_relaxed); }
    protected:
        etl::string<kLockPasswordMaxLen> pwd; /**< 密码明文 (定容) */
        etl::atomic_bool is_lock{true};       /**< 锁定状态 (原子) */
    };
}
#endif
