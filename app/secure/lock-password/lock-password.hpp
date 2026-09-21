/**
 * @file lock-password.hpp
 * @author H-000-H
 * @brief  锁屏密码: 常驻数据, 跟锁屏页面没有生命周期关系 —— 页面删了它还在
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
    constexpr std::uint8_t k_lock_password_max_len = 16;

    class LockPassword
    {
    public:
        LockPassword() = default;  /**< 默认构造 */
        ~LockPassword() = default; /**< 默认析构 */
        LockPassword(const LockPassword&) = delete;            /**< 禁用拷贝构造 */
        LockPassword& operator=(const LockPassword&) = delete; /**< 禁用拷贝赋值 */
        LockPassword(LockPassword&&) = delete;                 /**< 禁用移动构造 */
        LockPassword& operator=(LockPassword&&) = delete;      /**< 禁用移动赋值 */

        /** @brief 获取锁屏密码单例 */
        static LockPassword& get_instance()
        {
            static LockPassword instance;
            return instance;
        }

        /**
         * @brief 设/改密码
         * @param[in] text const etl::string_view& 新密码
         * @return bool 空 或 超长(> k_lock_password_max_len) 返回 false
         * @note  掉电不丢加 flash 操作
         */
        bool set(const etl::string_view& text);

        /**
         * @brief 比对输入是否与已设密码一致
         * @param[in] text const etl::string_view& 待校验的输入
         * @return bool 对得上返回 true; 密码为空(还没设过)时一律 false
         */
        bool match(const etl::string_view& text) const;

        /** @brief 是否尚未设置密码 */
        bool empty() const { return this->pwd.empty(); }

        /** @brief 读取当前锁定状态; true = 已锁定 */
        bool is_locked() const { return this->is_lock.load(etl::memory_order_relaxed); }

        /**
         * @brief 设置锁定状态
         * @param[in] locked bool true 锁定, false 解锁
         */
        void set_locked(bool locked) { this->is_lock.store(locked, etl::memory_order_relaxed); }

    protected:
        etl::string<k_lock_password_max_len> pwd;           /**< 密码明文 (定容) */
        etl::atomic_bool                     is_lock{true}; /**< 锁定状态 (原子) */
    };
}
#endif
