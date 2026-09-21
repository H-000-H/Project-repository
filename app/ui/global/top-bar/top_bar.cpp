/**
 * @file top_bar.cpp
 * @author H-000-H
 * @brief 顶栏外壳实现: flex 排版 + 项表增删改查
 * @note  接口说明见 top_bar.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "top_bar.hpp"
#include "home.hpp"
#include "system_log.h"
#include <cstring>
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/layouts/lv_flex.h>
#include <lvgl/widgets/lv_label.h>

namespace ui
{
    constexpr const char* const k_tag             = "top_bar";
    /* 条底默认全透明: 让下面的壁纸透出来, 只留子项(文字/图标) */
    constexpr lv_opa_t          k_default_bar_opa = LV_OPA_TRANSP;
    constexpr std::int32_t      k_bar_height_div  = 10; /* 条高 = 屏高 / 10 */
    constexpr const lv_font_t*  k_bar_font        = LV_FONT_DEFAULT_MONTSERRAT_12;

    /**
     * @brief 建一个"纯排版"flex 行: 去掉所有装饰, 只负责排列子项
     * @param[in] parent lv_obj_t* 父对象
     * @return lv_obj_t* 新建的 flex 行; 分配失败返回 nullptr
     */
    static lv_obj_t* make_row(lv_obj_t* parent)
    {
        lv_obj_t* row = lv_obj_create(parent);
        if (row == nullptr)
        {
            return nullptr;
        }
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(row, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        return row;
    }

    bool TopBar::create(lv_display_t* disp)
    {
        if (this->bar != nullptr)
        {
            return true;
        }
        if (disp == nullptr)
        {
            MT_LOG_ERROR(k_tag, "create: disp 为空, 拒绝 (不用默认屏兜底, 多屏会挂错)");
            return false;
        }
        lv_obj_t* screen = lv_display_get_screen_active(disp);
        if (screen == nullptr)
        {
            MT_LOG_ERROR(k_tag, "active screen 为空");
            return false;
        }

        this->bar = lv_obj_create(screen);
        if (this->bar == nullptr)
        {
            return false;
        }
        lv_obj_set_size(this->bar, LV_PCT(100), lv_obj_get_height(screen) / k_bar_height_div);
        lv_obj_align(this->bar, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_set_style_bg_opa(this->bar, k_default_bar_opa, LV_PART_MAIN);
        lv_obj_set_style_border_width(this->bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(this->bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(this->bar, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(this->bar, 0, LV_PART_MAIN);

        lv_obj_set_flex_flow(this->bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(this->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        this->head_row = make_row(this->bar);
        this->tail_row = make_row(this->bar);
        if ((this->head_row == nullptr) || (this->tail_row == nullptr))
        {
            return false;
        }
        lv_obj_set_flex_grow(this->head_row, 1); /* 左段弹性撑开: 占满除右段外的剩余宽度 */
        return true;
    }

    bool TopBar::add_item(const char* name, std::int32_t width, bool is_head)
    {
        /* etl::string 赋值会静默截断, 超长直接拒绝而不是存半个 key 进去 */
        if ((name == nullptr) || (std::strlen(name) > k_item_name_len))
        {
            MT_LOG_ERROR(k_tag, "项名非法或超过 %u 字节", static_cast<unsigned>(k_item_name_len));
            return false;
        }
        if (this->items.full())
        {
            MT_LOG_WARN(k_tag, "项表已满 (%u)", static_cast<unsigned>(k_max_items));
            return false;
        }

        lv_obj_t* obj = this->create_item_label(is_head, name, width);
        if (obj == nullptr)
        {
            return false; /* 控件没建起来就不入表, 不留半条记录 */
        }

        Item item;
        item.name    = name;
        item.obj     = obj;
        item.is_head = is_head;
        this->items.push_back(item);
        return true;
    }

    bool TopBar::remove_item(const char* name)
    {
        bool removed = false;
        /* erase 会让所有迭代器失效, 所以每轮重新 find —— 顺带支持同名多项 */
        for (ItemList::iterator it = this->find(name); it != this->items.end(); it = this->find(name))
        {
            lv_obj_delete(it->obj); /* flex 自己会补上空位, 不用 relayout */
            this->items.erase(it);
            removed = true;
        }
        return removed;
    }

    bool TopBar::set_item_color(const char* name, lv_color_t color)
    {
        ItemList::iterator it = this->find(name);
        if (it == this->items.end())
        {
            return false;
        }
        lv_obj_set_style_text_color(it->obj, color, LV_PART_MAIN);
        return true;
    }

    bool TopBar::set_item_opa(const char* name, lv_opa_t opa)
    {
        ItemList::iterator it = this->find(name);
        if (it == this->items.end())
        {
            return false;
        }
        lv_obj_set_style_opa(it->obj, opa, LV_PART_MAIN);
        return true;
    }

    bool TopBar::set_bar_color(lv_color_t color)
    {
        if (this->bar == nullptr)
        {
            return false;
        }
        lv_obj_set_style_bg_color(this->bar, color, LV_PART_MAIN);
        return true;
    }

    bool TopBar::set_bar_opa(lv_opa_t opa)
    {
        if (this->bar == nullptr)
        {
            return false;
        }
        lv_obj_set_style_opa(this->bar, opa, LV_PART_MAIN);
        return true;
    }

    bool TopBar::hide()
    {
        if (this->bar == nullptr)
        {
            return false;
        }
        lv_obj_set_hidden(this->bar, true);
        return true;
    }

    bool TopBar::show()
    {
        if (this->bar == nullptr)
        {
            return false;
        }
        lv_obj_set_hidden(this->bar, false);
        return true;
    }

    TopBar::ItemList::iterator TopBar::find(const char* name)
    {
        if (name == nullptr)
        {
            return this->items.end();
        }
        for (ItemList::iterator it = this->items.begin(); it != this->items.end(); ++it)
        {
            if (it->name == name)
            {
                return it;
            }
        }
        return this->items.end();
    }

    lv_obj_t* TopBar::create_item_label(bool is_head, const char* text, std::int32_t width)
    {
        lv_obj_t* parent = is_head ? this->head_row : this->tail_row;
        if (parent == nullptr)
        {
            MT_LOG_ERROR(k_tag, "条还没建, 先 create()");
            return nullptr;
        }
        lv_obj_t* label = lv_label_create(parent);
        if (label == nullptr)
        {
            return nullptr;
        }
        /* LVGL 内部拷贝文本, 不吊在调用方指针上 */
        lv_label_set_text(label, text);
        /* 高度一律跟条高; 宽度给了就固定像素, 没给按内容自适应 */
        lv_obj_set_size(label, (width > 0) ? width : LV_SIZE_CONTENT, LV_PCT(100));
        lv_obj_set_style_text_color(label, HomeColor::k_white, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, k_bar_font, LV_PART_MAIN);
        if (width > 0)
        {
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);                       /* 放不下打点省略 */
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN); /* 固定宽度时居中 */
        }
        return label;
    }
}
