/**
 * @file setting_main.cpp
 * @author H-000-H
 * @brief 设置页实现
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "setting_main.hpp"
#include "lvgl.h"
#include "system_log.h"
#include "../base/setting_base.hpp"
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
                return; 
            }
        }
        else
        {
            lv_obj_t* root = setting_base::create_page(parent);
            if (root == nullptr)
            {
                MT_LOG_ERROR(k_tag, "create_page failed");
                return;
            }
            this->set_root(root);
            create_scroll();
            create_option_blue();
            create_option_net();
            create_option_power();
        }
    }

    void SettingMain::exit()
    {
        this->destroy_widgets();
    }

    bool SettingMain::create_scroll()
    {
        if (this->scroll != nullptr)
        {
            MT_LOG_ERROR(k_tag, "create_scroll: scroll has been created");
            return true;
        }
        this->scroll = setting_base::create_scroll(get_root());
        if (this->scroll == nullptr)
        {
            MT_LOG_ERROR(k_tag, "create_scroll failed");
            return false;
        }
        return true;
    }

    void SettingMain::destroy_widgets()
    {
        destroy_root();
        /* 子控件lvgl自己释放 */
        this->scroll = nullptr;
        this->option_blue = nullptr;
        this->option_blue_text = nullptr;
        this->option_net = nullptr;
        this->option_net_text = nullptr;
        this->option_power = nullptr;
        this->option_power_text = nullptr;
    }

    bool SettingMain::create_option_blue()
    {
        if (this->scroll == nullptr)
        {
            return false;
        }
        return setting_base::create_option(this->scroll, "blue or other link", LV_PCT(90), LV_PCT(15),
                                           &this->option_blue, &this->option_blue_text);
    }

    bool SettingMain::create_option_net()
    {
        if (this->scroll == nullptr)
        {
            return false;
        }
        return setting_base::create_option(this->scroll, "net", LV_PCT(90), LV_PCT(15),
                                           &this->option_net, &this->option_net_text);
    }

    bool SettingMain::create_option_power()
    {
        if (this->scroll == nullptr)
        {
            return false;
        }
        return setting_base::create_option(this->scroll, "power", LV_PCT(90), LV_PCT(15),
                                           &this->option_power, &this->option_power_text);
    }
}