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
    constexpr const char* const k_tag = "setting_main";

    SettingMain::SettingMain(App& app) : Page(PageQuit::HIDE, "setting"), app(app)
    {

    }

    SettingMain::~SettingMain()
    {
        this->destroy_root();
    }

    /**
     * @brief 拆树: 本页只有一棵树, 交给基类连登记一起清掉
     * @note  只允许 exit 调用; 建树中途失败的回滚走 destroy_root, 不走这里
     */
    void SettingMain::destroy_widgets()
    {
        this->destroy_root();
        this->scroll = nullptr; /* 树连子节点一起没了, 指针跟着清, 不留悬空 */
    }

    /**
     * @brief 进入页面: 已在屏上直接返回, 隐藏中的树亮出来复用, 都没有才新建
     * @param[in] parent lv_obj_t* 挂载父对象
     * @note  Page::enter 要求幂等, 否则每次进来都会多出一棵树
     */
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

    bool SettingMain::create_widgets(lv_obj_t* parent)
    {
        return true;
    }
}
