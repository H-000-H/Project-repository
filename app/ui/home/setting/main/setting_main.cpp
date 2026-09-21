/**
 * @file setting_main.cpp
 * @author H-000-H
 * @brief 设置页实现
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "setting_main.hpp"
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
    constexpr const char* const k_tag = "setting_main";

    /* Page 基类没有默认构造: 注册名 + 默认收场方式必须在这里给 (运行期可用 set_quit_mode 改) */
    SettingMain::SettingMain(App& app) : Page(PageQuit::HIDE, "setting"), app(app)
    {
    }

    SettingMain::~SettingMain()
    {
        this->destroy_widgets();
    }

    void SettingMain::destroy_widgets()
    {
        /* TODO(你自己写): 删树要走基类的 destroy_root(), 别直接 lv_obj_delete(panel) ——
           基类那份 root 不清掉的话 has_widgets() 会说谎, 下次 enter 就去取消隐藏一块没了的内存 */
    }

    void SettingMain::enter(lv_obj_t* parent)
    {
        if (parent == nullptr)
        {
            return;
        }
        if (!this->create_widgets(parent))
        {
            return;
        }
    }

    void SettingMain::exit()
    {
        this->destroy_widgets();
    }

    bool SettingMain::create_widgets(lv_obj_t* parent)
    {
        if (!parent)
        {
            MT_LOG_ERROR(k_tag, "create_widgets: parent is null");
            return false;
        }

        this->panel = lv_obj_create(parent);
        if (!this->panel)
        {
            MT_LOG_ERROR(k_tag, "create_widgets: panel is null");
            return false;
        }
        lv_obj_t* root = lv_obj_create(this->panel);
        if (!root)
        {
            return false;
        }
        this->set_root(root);
        lv_obj_set_size(root, LV_PCT(k_page_default_width), LV_PCT(k_page_default_height));

        return false;
    }
}
