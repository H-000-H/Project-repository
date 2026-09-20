/**
 * @file lock.hpp
 * @author H-000-H
 * @brief  锁屏页面(挂在桌面容器下的子页面), 继承骨架与 top-bar 一致
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOCK_HPP
#define LOCK_HPP
#include "etl/string.h"
#include "home.hpp"
#include "lock-password.hpp"          /* 密码常驻在 app::LockPassword, 不跟这个页面一起生死 */
#include "lvgl/lvgl.h"
#include <cstdint>
#include <etl/vector.h>
#include <lvgl/core/lv_style.h>
#include <lvgl/display/lv_display.h>
namespace ui
{
    class LockViewModule
    {
    public:
        /**
         * @brief 构造: 初始化玻璃卡片样式 (lv_style_init 只做 memzero)
         */
        LockViewModule() { lv_style_init(&this->style_glass); }
        /**
         * @brief 析构 (本类唯一一处删除): 撤提示定时器 → 删锁屏树 → 放 style 属性表
         */
        ~LockViewModule()
        {
            /*  先撤提示的一次性定时器: 它 user_data 指着本对象, 不撤掉下一轮会回来写已释放内存 */
            if(this->tip_timer != nullptr)
            {
                lv_timer_delete(this->tip_timer);
                this->tip_timer = nullptr;
            }
            /* 删这棵树: lv_obj_delete(lock_screen)  */
            if(this->lock_screen != nullptr)
            {
                lv_obj_delete(this->lock_screen);
                this->lock_screen = nullptr;
            }
            this->textarea      = nullptr;  
            this->lock_show_tip = nullptr;
            /*  最后放 style 的属性表: LVGL 对象里存的是这个 style 的地址 */
            lv_style_reset(&this->style_glass);
        }
        /**
         * @brief 建锁屏面板
         * @param parent 挂到谁身上(一般是 Desktop::GetInstance().GetScreen())
         * @note  面板自带半透明底 + 居中偏移, 内部自己管文字
         * @return 成功(或已建)返回 true, parent 为空或创建失败返回 false
         */
        bool CreateLockScreen(lv_obj_t* parent);
        /**
         * @brief  建/更新锁屏提示文字, 并挂一个 1500ms 一次性定时器到期自动删除
         * @param  text  提示文字
         * @param  color 文字颜色 (默认红色)
         * @return 成功返回 true, 锁屏面板未建或标签创建失败返回 false
         */
        bool CreateLockShowTip(const char* text,lv_color_t color = HomeColor::Red);
        /**
         * @brief  更新锁屏提示文字
         * @param  text 新文字
         * @return 成功返回 true, 提示标签未创建返回 false
         */
        bool SetLockText(const char* text);
        /**
         * @brief  创建密码输入框
         * @return 成功返回 true, 锁屏面板未建返回 false
         */
        bool CreateLockTextArea();
        /**
         * @brief  获取锁屏面板对象
         * @return 锁屏面板指针 (未创建为 nullptr)
         */
        lv_obj_t* GetLockScreen() const { return this->lock_screen; }
        /**
         * @brief  提示还在屏上吗
         * @return true = 提示标签还在(1.5s 的一次性定时器还没把它收走)
         * @note   给页面拥有者用: 得等提示收走再删页面, 否则那条提示一帧都画不出来
         */
        bool IsTipShowing() const { return this->lock_show_tip != nullptr; }
    protected:
        /**
         * @brief 提示到期回调: 删除提示标签并清定时器句柄
         * @param timer 触发的一次性定时器 (user_data 指向本对象)
         * @note 做成静态成员才能改到下面这些 protected 成员(定时器的 user_data 传 this)
         */
        static void time_delete_lock_text_cb(lv_timer_t* timer);
        lv_obj_t* lock_screen = nullptr;  /*锁屏组件*/
        lv_obj_t* textarea    = nullptr;  /*输入框*/
        lv_obj_t* lock_show_tip   = nullptr;  /*锁屏组件的文字父组件为锁屏屏幕*/
        lv_timer_t* tip_timer = nullptr;      /*提示的一次性删除定时器: 析构必须撤掉它*/
        lv_style_t style_glass;               /*玻璃卡片样式: 做成成员, 别的函数也能拿到/复用*/
    };

    class LockDataModule
    {
    public:
        LockDataModule() = default;  /**< 默认构造 */
        ~LockDataModule() = default; /**< 默认析构 */
    protected:

    };

    class LockControllerModule : public LockViewModule, public LockDataModule
    {
    public:
        LockControllerModule() = default;  /**< 默认构造 */
        ~LockControllerModule() = default; /**< 默认析构 */
        /**
         * @brief  校验输入密码: 匹配则收起提示并删除锁屏, 不匹配则弹错误提示
         * @param  input 待校验的输入密码
         * @return 解锁成功返回 true, 密码错误返回 false
         */
        bool Unlock(const etl::string<app::kLockPasswordMaxLen>& input);
                /**
         * @brief 外部删除入口(CreateLockScreen 的反面): 解锁完/页面切走时调它
         * @note  删除动作全在 ~LockViewModule 里, 这里只是把析构调起来, 所以对象必须是 new 出来的。
         *        调用后指针立刻失效, 调用方自己把指针置空, 别再来第二遍(否则二次 delete)。
         *        必须写在这个最外层类里: 拿 LockControllerModule 或 LockViewModule 的指针去 delete,
         *        因为析构非虚 + 多继承, 只会析构子对象, 是 UB。
         */
        void DeleteLockScreen() { delete this; }
    protected:   
    };

    class Lock : public LockControllerModule
    {
    public:
        Lock() = default;  /**< 默认构造 */
        ~Lock() = default; /**< 默认析构 */
        Lock(const Lock&) = delete;            /**< 禁用拷贝构造 */
        Lock& operator=(const Lock&) = delete; /**< 禁用拷贝赋值 */
        Lock(Lock&&) = delete;                 /**< 禁用移动构造 */
        Lock& operator=(Lock&&) = delete;      /**< 禁用移动赋值 */
    };
}
#endif
