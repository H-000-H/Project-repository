/**
 * @file lock_screen.cpp
 * @author H-000-H
 * @brief 锁屏页面实现
 * @note  接口说明见 lock_screen.hpp, 本文件只留实现要点。
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
    constexpr const char* const k_page_name   = "lock"; /**< 页表注册名 */
    constexpr const char* const k_unlock_hint = "Unlock with any operation";
    constexpr const char* const k_tip_ok      = "Welcome";
    constexpr const char* const k_tip_fail    = "password error please rewrite";
    constexpr std::uint32_t     k_tip_show_ms = 1500U; /**< 提示停留时长 */

    LockScreen::LockScreen(App& app) : Page(PageQuit::DESTROY, k_page_name), app(app)
    {
        /* lv_style_init 只做 memzero, 不依赖 lv_init, 静态构造期调用安全 */
        lv_style_init(&this->style_glass);
    }

    LockScreen::~LockScreen()
    {
        /* 只拆自己的控件树, 不去碰外壳: 静态析构期 LVGL 运行时可能已经先没了 */
        this->destroy_widgets();
        lv_style_reset(&this->style_glass);
    }

    void LockScreen::enter(lv_obj_t* parent)
    {
        /* 收场方式运行期可改, 所以"树还在"有两种含义: 本来就在屏上(幂等直接回),
           或上次按 HIDE 收的场(亮出来复用, 但上次的输入不能留) */
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
    }

    void LockScreen::exit()
    {
        this->app.get_desktop().set_top_text(""); /* 收走桌面提示 */
        this->destroy_widgets();
    }

    void LockScreen::on_submit(const char* input)
    {
        /* 非 LOCKED(提示还在屏上 / 已解锁)时忽略重复提交, 防连按打乱状态机 */
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
        /* 只有成功路径的提示到期才推进到 UNLOCKED; 先 clear_tip 再改状态:
           is_finished() 为 true 时提示必然已经不在屏上 */
        if (this->state == LockState::UNLOCKING)
        {
            this->state = LockState::UNLOCKED;
        }
    }

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

        /* 玻璃卡片样式: set 系列会 lv_malloc, 必须在 lv_init 之后跑 (enter 在 init 之后调用) */
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
            this->destroy_root(); /* 半棵树不能留在屏上: 基类登记跟着清掉 */
            return false;
        }
        lv_obj_set_size(this->textarea, LV_PCT(100), LV_PCT(100));
        lv_obj_align(this->textarea, LV_ALIGN_TOP_MID, 0, 0);
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
        /* 删根对象走基类: 它同时清掉自己的登记; textarea / tip_label 是根的子节点跟着走,
           LVGL 在 lv_obj_destruct 里会把子对象从焦点组摘掉 */
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
