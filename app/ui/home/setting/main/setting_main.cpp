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
#include <lvgl/core/lv_area.h>
#include <lvgl/core/lv_group.h>
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_scroll.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/core/lv_style.h>
#include <lvgl/core/lv_style_gen.h>
#include <lvgl/core/lv_timer.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/layouts/lv_flex.h>
#include <lvgl/layouts/lv_layout.h>
#include <lvgl/widgets/lv_label.h>
#include <lvgl/widgets/lv_textarea.h>

namespace ui
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    // 本页日志标签
    constexpr const char* const k_tag = "setting_main";

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    SettingMain::SettingMain(App& app) : Page(PageQuit::HIDE, "setting"), app(app)
    {

    }

    SettingMain::~SettingMain()
    {
        this->destroy_root();
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    void SettingMain::enter(lv_obj_t* parent)
    {
        if (this->has_widgets())
        {
            if (!this->reuse_root())
            {
                return; /* 本来就在屏上 (重复 enter) */
            }
        }
        else if (!this->create_widgets(parent))
        {
            MT_LOG_ERROR(k_tag, "create_widgets failed");
            return;
        }
    }

    void SettingMain::exit()
    {
        this->destroy_widgets();
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool SettingMain::create_widgets(lv_obj_t* parent)
    {
        return true;
    }

    void SettingMain::destroy_widgets()
    {
        this->destroy_root();
        this->scroll = nullptr; /* 树连子节点一起没了, 指针跟着清, 不留悬空 */
    }
}
