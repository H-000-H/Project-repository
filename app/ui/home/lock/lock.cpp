/**
 * @file lock.cpp
 * @author H-000-H
 * @brief  锁屏页面实现
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "lock.hpp"
#include "dektop.hpp"
#include "home.hpp"
#include "lock-password.hpp"
#include "system_log.h"
#include <atomic>
#include <cstddef>
#include <lvgl/config/lv_conf_internal.h>
#include <lvgl/core/lv_area.h>
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/core/lv_obj_tree.h>
#include <lvgl/core/lv_style.h>
#include <lvgl/core/lv_timer.h>
#include <lvgl/display/lv_display.h>
#include <cstdint>
#include <lvgl/draw/lv_color.h>
#include <lvgl/lv_types.h>
#include <lvgl/widgets/lv_label.h>
#include <lvgl/widgets/lv_textarea.h>
namespace ui
{
    constexpr const char*const kTag = "lock";

    /**
     * @brief  建锁屏面板: 玻璃卡片样式 + 居中偏移
     * @param  parent 挂载的父对象 (一般是桌面 screen)
     * @return 成功(或已建)返回 true, parent 为空或创建失败返回 false
     */
    bool LockViewModule::CreateLockScreen(lv_obj_t* parent)
    {
        if(this->lock_screen != nullptr)
            return true;
        if(parent == nullptr)
        {
            MT_LOG_ERROR(kTag,"parent 为空, 先 CreateDesktop() 拿 screen");
            return false;
        }
        this->lock_screen = lv_obj_create(parent);
        if(this->lock_screen == nullptr)
            return false;
        lv_obj_set_size(this->lock_screen,LV_PCT(60),LV_PCT(8));
        /* 玻璃卡片样式: style_glass 是成员(声明在 LockViewModule), init 在构造里;
         * 下面这些 set 会 lv_malloc, 必须在 lv_init 之后跑 —— 靠开头的 lock_screen 判空保证只跑一次 */
        lv_style_set_bg_color(&this->style_glass, lv_color_white());
        lv_style_set_bg_opa(&this->style_glass, LV_OPA_40);      /* lv_opa_t 是 0~255 的实际值, 不是百分比 */
        lv_style_set_blur_radius(&this->style_glass, 4);        /* 模糊半径 8~24 之间调 */
        lv_style_set_blur_backdrop(&this->style_glass, true);    /* 开启背景模糊 */
        lv_style_set_border_color(&this->style_glass, lv_color_white());
        lv_style_set_border_width(&this->style_glass, 1);
        lv_style_set_border_opa(&this->style_glass, LV_OPA_60);
        lv_style_set_shadow_color(&this->style_glass, lv_color_black());
        lv_style_set_shadow_opa(&this->style_glass, LV_OPA_30); 
        lv_style_set_shadow_width(&this->style_glass, 8);
        /* 不挂上去前面那些设置一个都不生效; 背景统一交给 style, 别再在对象上重复设 bg_* */
        lv_obj_add_style(this->lock_screen, &this->style_glass, LV_PART_MAIN);
        lv_obj_align(this->lock_screen,LV_ALIGN_TOP_MID,0,40);
        lv_obj_set_style_pad_all(this->lock_screen,8,LV_PART_MAIN);
        return true;
    }

    /**
     * @brief 提示到期回调: 删除提示标签并清定时器句柄
     * @param timer 触发的一次性定时器 (user_data 指向本对象)
     */
    void LockViewModule::time_delete_lock_text_cb(lv_timer_t* timer)
    {
        LockViewModule* self = static_cast<LockViewModule*>(lv_timer_get_user_data(timer));
        if(self == nullptr)
            return;
        /* repeat_count==1 + auto_delete: 这轮跑完 LVGL 自己删定时器*/
        self->tip_timer = nullptr;
        if(self->lock_show_tip != nullptr)
        {
            lv_obj_delete(self->lock_show_tip);
            self->lock_show_tip = nullptr;
        }
    }

    /**
     * @brief  建/更新锁屏提示文字, 并挂一个 1500ms 一次性定时器到期自动删除
     * @param  text  提示文字
     * @param  color 文字颜色
     * @return 成功返回 true, 锁屏面板未建或标签创建失败返回 false
     */
    bool LockViewModule::CreateLockShowTip(const char* text,lv_color_t color)
    {
        if(this->lock_screen == nullptr)
        {
            MT_LOG_ERROR(kTag,"lock screen 还没建, 先 CreateLockScreen()");
            return false;
        }
        if(this->lock_show_tip == nullptr)
            this->lock_show_tip =  lv_label_create(this->lock_screen); //顶层弹窗，盖在所有控件上面
        if(this->lock_show_tip == nullptr)
            return false;
        lv_label_set_text(this->lock_show_tip, text);
        lv_obj_set_style_bg_color(this->lock_show_tip, lv_color_black(), LV_PART_MAIN);;
        lv_obj_set_style_pad_all(this->lock_show_tip, 10, LV_PART_MAIN);
        lv_obj_set_style_text_font(this->lock_show_tip,LV_FONT_DEFAULT_MONTSERRAT_16,LV_PART_MAIN);
        lv_obj_center(this->lock_show_tip);
        lv_obj_set_hidden(this->lock_show_tip, false);  
        lv_obj_set_style_text_font(this->lock_show_tip,LV_FONT_DEFAULT_MONTSERRAT_12,LV_PART_MAIN);
        lv_obj_set_style_text_color(this->lock_show_tip,color,LV_PART_MAIN);

        /* 重开一次先把上一个撤掉 */
        if(this->tip_timer != nullptr)
        {
            lv_timer_delete(this->tip_timer);
            this->tip_timer = nullptr;
        }
        /* user_data 传 this:改 lock_show_tip / tip_timer */
        this->tip_timer = lv_timer_create(time_delete_lock_text_cb, 1500, this);
        if(this->tip_timer == nullptr)
            return true;                  
        lv_timer_set_repeat_count(this->tip_timer,1);   /* 一次性 */
        return true;
    }

    /**
     * @brief  更新锁屏提示文字
     * @param  text 新文字
     * @return 成功返回 true, 提示标签未创建返回 false
     */
    bool LockViewModule::SetLockText(const char* text)
    {
        if(this->lock_show_tip == nullptr)
            return false;
        lv_label_set_text(this->lock_show_tip,text);
        return true;
    }

    /**
     * @brief  创建密码输入框
     * @return 成功返回 true, 锁屏面板未建返回 false
     */
    bool LockViewModule::CreateLockTextArea()
    {
        if(this->lock_screen==nullptr)
            return false;

        this->textarea = lv_textarea_create(this->lock_screen);
        lv_obj_align(this->textarea,LV_ALIGN_TOP_MID,0,0);
        lv_obj_set_size(this->textarea,LV_PCT(100),LV_PCT(15));
        return true;
    }

    /**
     * @brief  校验输入密码: 匹配则弹欢迎提示(1.5s 后自己收走), 不匹配则弹错误提示
     * @param  input 待校验的输入密码
     * @return 解锁成功返回 true, 密码错误返回 false
     * @note   这里只改状态 + 显示提示, 不删本页面: 页面删早了提示一帧都画不出来。
     *         页面生死交给持有指针的人(见 app_ui: "解锁完成且提示已收走 → DeleteLockScreen()")
     */
    bool LockControllerModule::Unlock(const etl::string<app::kLockPasswordMaxLen>& input)
    {
        auto& store = app::LockPassword::GetInstance();
        const bool matched = store.Match(input);
        store.SetLocked(!matched);
        if(matched)
        {
            /* CreateLockShowTip 自己就是"没有就建": 别判 lock_show_tip 非空(它可能已被 1.5s 定时器删掉),
             * 也别紧接着 hidden —— 刚显示就隐藏等于没有 */
            this->CreateLockShowTip("Welcome", HomeColor::Green);
        }
        else
        {
            this->CreateLockShowTip("password error please rewrite");
            return false;
        }
        
        //TODO 做完整的逻辑去删顶层应该是触发就hidden toptext明天改
        auto& dektop = ui::Desktop::GetInstance();
        auto toptext =  dektop.GetTopText();
        lv_obj_delete(toptext);
        return matched;                 /* 页面不在这儿删, 理由见上面的 @note */
    }

}
