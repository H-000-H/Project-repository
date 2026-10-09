/**
 * @file desktop.cpp
 * @author H-000-H
 * @brief 桌面外壳实现: 直接操作 LVGL 控件树, 无中间层
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "desktop.hpp"
#include "home.hpp"
#include "system_log.h"
#include "ui_app.hpp"
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_event.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_image_dsc.h>
#include <lvgl/layouts/lv_grid.h>
#include <lvgl/widgets/lv_image.h>
#include <lvgl/widgets/lv_label.h>

LV_IMG_DECLARE(desktop_320x240);

namespace ui
{
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    constexpr const char* const k_tag           = "desktop";
    constexpr const lv_font_t*  k_top_text_font = LV_FONT_DEFAULT_MONTSERRAT_12;
    // 图标字形/名字字号 (本工程只开了 12 与 16)
    constexpr const lv_font_t*  k_app_icon_font = LV_FONT_DEFAULT_MONTSERRAT_16;
    constexpr const lv_font_t*  k_app_name_font = LV_FONT_DEFAULT_MONTSERRAT_12;
    // 栅格距屏顶 (百分比, 要避开 1/10 屏高的顶栏)
    constexpr int32_t           k_app_grid_top_pct = 15;
    /* 栅格整体占屏高的比例 + 上限(px): 屏小按比例缩, 屏大到手机/平板级就卡住, 图标不再跟着长。
       行高 = 栅格高 / 行数, 钳住这一个值, 里面的图标/文字全都被它限制住 */
    constexpr int32_t           k_app_grid_height_pct = 70;
    constexpr int32_t           k_app_grid_max_height = 240;
    // 栅格行数 (由槽位上限和列数推)
    constexpr int32_t           k_app_row_count = (k_app_bar_max_count + k_app_bar_columns - 1) / k_app_bar_columns;
    // 图标占格子高度的比例; 名字是它下面的一行, 高度交给字体, 不占比例
    constexpr int32_t           k_app_icon_height_pct = 70;
    // 名字一行的高度预算 (montserrat_12 约 16px): 图标吃完必须还剩得下它
    constexpr int32_t           k_app_name_line_px = 16;
    static_assert((k_app_icon_height_pct > 0) && (k_app_icon_height_pct < 100),
                  "图标占比要在 0~100 之间, 剩下的才留给名字那行");

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    namespace
    {
        /* 栅格模板: lv_obj_set_grid_dsc_array 只存指针, 数组必须静态常驻 */
        const int32_t k_app_col_dsc[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
        /* 行轨道等分栅格高度: 行高 = 栅格高 / 3, 跟着屏缩放 */
        const int32_t k_app_row_dsc[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

        /* 数组条数必须和常量对上 (+1 是 LV_GRID_TEMPLATE_LAST), 不同步会让
           超出的图标被 grid 塞到最后一行叠住 */
        static_assert((sizeof(k_app_col_dsc) / sizeof(k_app_col_dsc[0])) == static_cast<size_t>(k_app_bar_columns) + 1U,
                      "列模板数组条数要和 k_app_bar_columns 同步改");
        static_assert((sizeof(k_app_row_dsc) / sizeof(k_app_row_dsc[0])) == static_cast<size_t>(k_app_row_count) + 1U,
                      "行模板数组条数要和 k_app_row_count 同步改");
    } // namespace

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool Desktop::create(lv_display_t* disp)
    {
        if (this->screen != nullptr)
        {
            return true;
        }
        if (disp == nullptr)
        {
            MT_LOG_ERROR(k_tag, "create: disp 为空, 拒绝 (不用默认屏兜底, 多屏会挂错)");
            return false;
        }

        lv_obj_t* const scene = lv_display_get_screen_active(disp);

        this->wallpaper = lv_image_create(scene);
        if (this->wallpaper == nullptr)
        {
            MT_LOG_ERROR(k_tag, "wallpaper create failed");
            return false;
        }
        lv_image_set_src(this->wallpaper, &desktop_320x240);
        lv_obj_align(this->wallpaper, LV_ALIGN_TOP_LEFT, 0, 0);

        this->screen = lv_obj_create(scene);
        if (this->screen == nullptr)
        {
            MT_LOG_ERROR(k_tag, "screen create failed");
            return false;
        }
        lv_obj_set_size(this->screen, LV_PCT(100), LV_PCT(100));
        lv_obj_align(this->screen, LV_ALIGN_TOP_LEFT, 0, 0);
        /* 消除默认屏幕的边距与装饰, 只要一个透明容器 */
        lv_obj_set_style_pad_all(this->screen, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(this->screen, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(this->screen, LV_OPA_TRANSP, LV_PART_MAIN);

        /* 图标栅格: 早于任何页面建树 → 页面盖在它上面, 只剩外壳时图标才露出来 */
        this->app_grid = lv_obj_create(this->screen);
        if (this->app_grid == nullptr)
        {
            MT_LOG_ERROR(k_tag, "app grid create failed");
            return false;
        }
        /* 直接拿最底层的屏幕高度算*/
        const int32_t screen_h = lv_display_get_vertical_resolution(disp);
        int32_t       grid_h   = (screen_h * k_app_grid_height_pct) / 100;
        if (grid_h > k_app_grid_max_height)
        {
            grid_h = k_app_grid_max_height;
        }
        const int32_t row_h = grid_h / k_app_row_count;
        if (((row_h * (100 - k_app_icon_height_pct)) / 100) < k_app_name_line_px)
        {
            MT_LOG_WARN(k_tag, "行高 %d 放不下名字一行(%d): 调小 k_app_icon_height_pct 或调大 k_app_grid_height_pct",
                        static_cast<int>(row_h), static_cast<int>(k_app_name_line_px));
        }
        lv_obj_set_size(this->app_grid, LV_PCT(90), grid_h);
        lv_obj_align(this->app_grid, LV_ALIGN_TOP_MID, 0, LV_PCT(k_app_grid_top_pct));
        lv_obj_set_grid_dsc_array(this->app_grid, k_app_col_dsc, k_app_row_dsc);
        lv_obj_set_style_bg_opa(this->app_grid, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(this->app_grid, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(this->app_grid, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(this->app_grid, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(this->app_grid, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(this->app_grid, 8, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(this->app_grid, LV_SCROLLBAR_MODE_OFF);

        return true;
    }

    void Desktop::set_wallpaper(const lv_image_dsc_t& image)
    {
        if (this->wallpaper != nullptr)
        {
            lv_image_set_src(this->wallpaper, &image);
        }
    }

    void Desktop::set_top_text(const char* text)
    {
        if (text == nullptr)
        {
            return;
        }
        if ((this->top_text == nullptr) && !this->create_top_label())
        {
            return;
        }
        lv_label_set_text(this->top_text, text);
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool Desktop::create_top_label()
    {
        if (this->screen == nullptr)
        {
            MT_LOG_ERROR(k_tag, "screen 还没建, 先 create()");
            return false;
        }
        this->top_text = lv_label_create(this->screen);
        if (this->top_text == nullptr)
        {
            return false;
        }
        lv_obj_set_size(this->top_text, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_align(this->top_text, LV_ALIGN_TOP_MID, 0, LV_PCT(10));
        lv_obj_set_style_text_align(this->top_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(this->top_text, k_top_text_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(this->top_text, HomeColor::k_blue, LV_PART_MAIN);
        return true;
    }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
    bool Desktop::add_app_bar(const char* icon, const char* text, const char* page, const void* image_src)
    {
        if (this->app_grid == nullptr)
        {
            MT_LOG_ERROR(k_tag, "add_app_bar: grid not created, first try create()");
            return false;
        }
        if (text == nullptr)
        {
            return false;
        }
        /* 图标二选一: 都空是没图标, 都给是不知道用哪个, 都算调用错 */
        const bool has_image = (image_src != nullptr);
        const bool has_glyph = (icon != nullptr);
        if (has_image == has_glyph)
        {
            MT_LOG_ERROR(k_tag, "add_app_bar: icon 与 image_src 必须二选一");
            return false;
        }

        /* 找空槽: 槽位下标就是栅格里的位置 */
        uint8_t slot = k_app_bar_max_count;
        for (uint8_t i = 0; i < k_app_bar_max_count; ++i)
        {
            if (this->bars[i].cell == nullptr)
            {
                slot = i;
                break;
            }
        }
        if (slot == k_app_bar_max_count)
        {
            MT_LOG_WARN(k_tag, "add_app_bar: 槽位满(%u), \"%s\" 进不去",
                        static_cast<unsigned>(k_app_bar_max_count), text);
            return false;
        }

        AppBar& bar = this->bars[slot];
        bar.cell    = lv_obj_create(this->app_grid);
        if (bar.cell == nullptr)
        {
            return false;
        }
        lv_obj_set_grid_cell(bar.cell, LV_GRID_ALIGN_STRETCH, slot % k_app_bar_columns, 1,
                                      LV_GRID_ALIGN_STRETCH, slot / k_app_bar_columns, 1);
        lv_obj_set_layout(bar.cell, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(bar.cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bar.cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(bar.cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar.cell, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bar.cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar.cell, 0, LV_PART_MAIN);
        /* 整格才算一个可点区: 下面所有子对象都要摘除点击功能不然会被点击到*/

        /* image 在前 icon 在后: 只建被选中的那个, 另一个留 nullptr */
        if (has_image)
        {
            bar.image = lv_image_create(bar.cell);
            if (bar.image == nullptr)
            {
                return false;
            }
            lv_image_set_src(bar.image, image_src);
            /* 图标区 = 格子高的 k_app_icon_height_pct%; 位图默认按原尺寸画, 要 CONTAIN 才缩进去 */
            lv_obj_set_size(bar.image, LV_PCT(100), LV_PCT(k_app_icon_height_pct));
            lv_image_set_inner_align(bar.image, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_clickable(bar.image, false);
        }
        else
        {
            bar.icon = lv_label_create(bar.cell);
            if (bar.icon == nullptr)
            {
                return false;
            }
            lv_label_set_text(bar.icon, icon);
            /* 字形高度由字体定, 不给框: 靠格子的主轴居中把它压在名字上面 */
            lv_obj_set_width(bar.icon, LV_PCT(100));
            lv_obj_set_style_text_align(bar.icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_font(bar.icon, k_app_icon_font, LV_PART_MAIN);
            lv_obj_set_style_text_color(bar.icon, HomeColor::k_white, LV_PART_MAIN);
            lv_obj_set_clickable(bar.icon, false);
        }

        bar.name = lv_label_create(bar.cell);
        if (bar.name == nullptr)
        {
            return false;
        }
        /* 名字 = 图标下面一行, 高度交给字体, 不给百分比框:
           框一旦比字矮(行高被压小时就会), 字会被格子下边界裁掉 */
        lv_label_set_text(bar.name, text);
        lv_label_set_long_mode(bar.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(bar.name, LV_PCT(100));
        lv_obj_set_style_text_align(bar.name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(bar.name, k_app_name_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(bar.name, HomeColor::k_white, LV_PART_MAIN);
        lv_obj_set_clickable(bar.name, false);

        bar.text = text;
        bar.page = page;
        if (page != nullptr)
        {
            /* user_data 传 this: 回调里按被点的格子反查槽位 */
            lv_obj_add_event_cb(bar.cell, app_bar_clicked_cb, LV_EVENT_CLICKED, this);
        }
        if (!this->bars_visible)
        {
            lv_obj_set_hidden(bar.cell, true); /* 锁屏期间补加的图标也不显示 */
        }
        return true;
    }

    void Desktop::set_app_bar_visible(bool vis)
    {
        this->bars_visible = vis;
        for (uint8_t i = 0; i < k_app_bar_max_count; ++i)
        {
            lv_obj_t* const cell = this->bars[i].cell;
            if (cell == nullptr)
            {
                continue;
            }
            lv_obj_set_hidden(cell, !vis);
        }
    }

    void Desktop::app_bar_clicked_cb(lv_event_t* e)
    {
        auto* self = static_cast<Desktop*>(lv_event_get_user_data(e));
        if (self == nullptr)
        {
            return;
        }
        lv_obj_t* const cell = lv_event_get_current_target_obj(e);
        for (uint8_t i = 0; i < k_app_bar_max_count; ++i)
        {
            if (self->bars[i].cell != cell)
            {
                continue;
            }
            if (self->router == nullptr)
            {
                MT_LOG_ERROR(k_tag, "router 没注入, 图标的跳页请求丢弃");
                return;
            }
            /* 只排队: 真正的换页/拆树在下一轮 App::tick, 不在本回调栈上 */
            self->router->show_page(self->bars[i].page);
            return;
        }
    }
}
