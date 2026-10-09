/**
 * @file lock_screen.cpp
 * @author H-000-H
 * @brief 锁屏页面实现
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "lock_screen.hpp"
#include "home.hpp"
#include "system_log.h"
#include "ui_app.hpp"
#include <etl/string_view.h>
#include <lvgl/core/lv_group.h>
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/core/lv_style.h>
#include <lvgl/core/lv_timer.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/widgets/lv_label.h>
#include <lvgl/widgets/lv_textarea.h>

namespace ui
{
    constexpr const char* const k_tag         = "lock_screen";
    // 页表注册名
    constexpr const char* const k_page_name   = "lock";
    // 解锁后去哪: 页表里的名字, 由 SettingMain 自己注册
    constexpr const char* const k_next_page_name = "setting";
    constexpr const char* const k_unlock_hint = "Unlock with any operation";
    constexpr const char* const k_tip_ok      = "Welcome";
    constexpr const char* const k_tip_fail    = "password error please rewrite";
    // 提示停留时长
    constexpr std::uint32_t     k_tip_show_ms = 1500U;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    LockScreen::LockScreen(App& app) : Page(PageQuit::DESTROY, k_page_name), app(app)
    {
        lv_style_init(&this->style_glass);
    }

    LockScreen::~LockScreen()
    {
        this->destroy_widgets();
        lv_style_reset(&this->style_glass);
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void LockScreen::enter(lv_obj_t* parent)
    {
        if (this->has_widgets())
        {
            if (!this->reuse_root())
            {
                return;
            }
            if (this->textarea != nullptr)
            {
                lv_textarea_set_text(this->textarea, "");
            }
        }
        else if (!this->create_widgets(parent))
        {
            return;
        }

        this->state      = LockState::LOCKED;
        this->fail_count = 0U;

        this->app.get_desktop().set_top_text(k_unlock_hint);
        this->grab_focus();
        /* 锁屏期间藏掉桌面图标: 只能解锁, 进不了别的页 */
        this->app.get_desktop().set_app_bar_visible(false);
    }

    void LockScreen::exit()
    {
        this->app.get_desktop().set_top_text(""); /* 收走桌面提示 */
        this->app.get_desktop().set_app_bar_visible(true); /* 离开锁屏才把图标放回来 */
        this->destroy_widgets();
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void LockScreen::on_submit(const char* input)
    {
        if ((input == nullptr) || (this->state != LockState::LOCKED))
        {
            return;
        }

        if (this->store.match(etl::string_view(input)))
        {
            this->store.set_locked(false);
            this->state      = LockState::UNLOCKING;
            this->fail_count = 0U;
            this->show_tip(k_tip_ok, HomeColor::k_green);
            MT_LOG_INFO(k_tag, "unlock success");
        }
        else
        {
            this->store.set_locked(true);
            ++this->fail_count;
            this->show_tip(k_tip_fail, HomeColor::k_red);
            if (this->textarea != nullptr)
            {
                lv_textarea_set_text(this->textarea, "");
            }
            MT_LOG_WARN(k_tag, "unlock failed, count=%u", this->fail_count);
        }
    }

    void LockScreen::on_tip_timeout()
    {
        this->clear_tip();

        if (this->state == LockState::UNLOCKING)
        {
            this->state = LockState::UNLOCKED;
            this->app.show_page(k_next_page_name);
        }
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool LockScreen::create_widgets(lv_obj_t* parent)
    {
        if (parent == nullptr)
        {
            MT_LOG_ERROR(k_tag, "parent 为空, 外壳还没建好");
            return false;
        }

        lv_obj_t* root = lv_obj_create(parent);
        if (root == nullptr)
        {
            return false;
        }
        this->set_root(root); /* 根交给基类持有: 隐藏/复用/拆除都只认它 */
        lv_obj_set_size(root, LV_PCT(60), LV_PCT(8));
        lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
        lv_style_set_bg_color(&this->style_glass, lv_color_white());
        lv_style_set_bg_opa(&this->style_glass, LV_OPA_40);
        lv_style_set_blur_radius(&this->style_glass, 4);
        lv_style_set_blur_backdrop(&this->style_glass, true);
        lv_style_set_border_color(&this->style_glass, lv_color_white());
        lv_style_set_border_width(&this->style_glass, 1);
        lv_style_set_border_opa(&this->style_glass, LV_OPA_60);
        lv_style_set_shadow_color(&this->style_glass, lv_color_black());
        lv_style_set_shadow_opa(&this->style_glass, LV_OPA_30);
        lv_style_set_shadow_width(&this->style_glass, 8);
        lv_obj_add_style(root, &this->style_glass, LV_PART_MAIN);

        this->textarea = lv_textarea_create(root);
        if (this->textarea == nullptr)
        {
            this->destroy_root(); 
            return false;
        }
        lv_obj_set_size(this->textarea, LV_PCT(100), LV_PCT(100));
        lv_obj_align(this->textarea, LV_ALIGN_TOP_MID, 0, -LV_PCT(10));
        lv_textarea_set_password_mode(this->textarea, true);
        lv_textarea_set_one_line(this->textarea, true);
        /* user_data 传 this: 回调里拿回页面对象 */
        lv_obj_add_event_cb(this->textarea, enter_event_cb, LV_EVENT_READY, this);
        return true;
    }

    void LockScreen::destroy_widgets()
    {
        /* 先撤定时器: 它的 user_data 指着本对象, 不撤掉下一轮会回来写已释放的控件树 */
        if (this->tip_timer != nullptr)
        {
            lv_timer_delete(this->tip_timer);
            this->tip_timer = nullptr;
        }
        this->destroy_root();
        this->textarea  = nullptr;
        this->tip_label = nullptr;
    }

    void LockScreen::grab_focus()
    {
        lv_group_t* group = this->app.get_group();
        if ((group == nullptr) || (this->textarea == nullptr))
        {
            return;
        }
        lv_group_add_obj(group, this->textarea);
        lv_group_focus_obj(this->textarea);
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void LockScreen::show_tip(const char* text, lv_color_t color)
    {
        lv_obj_t* root = this->get_root();
        if (root == nullptr)
        {
            MT_LOG_ERROR(k_tag, "控件树还没建");
            return;
        }
        if (this->tip_label == nullptr)
        {
            this->tip_label = lv_label_create(root);
            if (this->tip_label == nullptr)
            {
                return;
            }
            lv_obj_center(this->tip_label);
            lv_obj_set_style_bg_color(this->tip_label, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_pad_all(this->tip_label, 10, LV_PART_MAIN);
            lv_obj_set_style_text_font(this->tip_label, LV_FONT_DEFAULT_MONTSERRAT_12, LV_PART_MAIN);
        }
        lv_label_set_text(this->tip_label, text);
        lv_obj_set_style_text_color(this->tip_label, color, LV_PART_MAIN);
        lv_obj_set_hidden(this->tip_label, false);

        /* 重开一次先把上一个撤掉 (连续输错时不会叠两个定时器) */
        if (this->tip_timer != nullptr)
        {
            lv_timer_delete(this->tip_timer);
            this->tip_timer = nullptr;
        }
        this->tip_timer = lv_timer_create(tip_timer_cb, k_tip_show_ms, this);
        if (this->tip_timer != nullptr)
        {
            lv_timer_set_repeat_count(this->tip_timer, 1); /* 一次性, 到期 LVGL 自删 */
        }
    }

    void LockScreen::clear_tip()
    {
        /* 定时器回调路径已把 tip_timer 置空(repeat_count=1, LVGL 自删),
           这里只可能删 show_tip 重开残留的定时器, 不会二次 delete */
        if (this->tip_timer != nullptr)
        {
            lv_timer_delete(this->tip_timer);
            this->tip_timer = nullptr;
        }
        if (this->tip_label != nullptr)
        {
            lv_obj_delete(this->tip_label);
            this->tip_label = nullptr;
        }
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void LockScreen::enter_event_cb(lv_event_t* e)
    {
        auto* self = static_cast<LockScreen*>(lv_event_get_user_data(e));
        if ((self == nullptr) || (self->textarea == nullptr))
        {
            return;
        }
        self->on_submit(lv_textarea_get_text(self->textarea));
    }

    void LockScreen::tip_timer_cb(lv_timer_t* t)
    {
        auto* self = static_cast<LockScreen*>(lv_timer_get_user_data(t));
        if (self == nullptr)
        {
            return;
        }
        /* 先置空再转发: on_tip_timeout 会调 clear_tip, 避免它对正在回调中的定时器二次 delete */
        self->tip_timer = nullptr;
        self->on_tip_timeout();
    }
}
