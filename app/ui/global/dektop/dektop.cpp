/**
 * @file dektop.cpp
 * @author H-000-H
 * @brief  桌面实现(壁纸 + 顶层文字(解锁自动消失)); 
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "dektop.hpp"                 
#include "home.hpp"
#include "lvgl/lvgl.h"
#include "system_log.h"
#include <lvgl/config/lv_conf_internal.h>
#include <lvgl/core/lv_area.h>
#include <lvgl/core/lv_obj.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/draw/lv_image_dsc.h>
#include <lvgl/lv_types.h>
#include <cstdint>
#include <lvgl/widgets/lv_image.h>
#include <lvgl/widgets/lv_label.h>
LV_IMG_DECLARE(dektop_320x240);
namespace ui 
{
    constexpr const char*const kTag = "dektop";
    /**
     * @brief  创建桌面: 壁纸 + 透明基准屏幕组件
     * @return 成功返回 true(1), 失败返回 false(0)
     */
    int DesktopViewModule::CreateDesktop()
    {
        this->image = lv_image_create(lv_screen_active());
        if(this->image == nullptr)
        {
            MT_LOG_ERROR(kTag,"image create fail");
            return false;
        }
        lv_image_set_src(this->image,&dektop_320x240);
        lv_obj_align(this->image,LV_ALIGN_TOP_LEFT,0,0);

        this->screen = lv_obj_create(lv_screen_active());
        if(this->screen == nullptr)
        {
            MT_LOG_ERROR(kTag,"dektop create fail");
            return false;
        }
        lv_obj_set_size(this->screen,LV_PCT(100),LV_PCT(100));
        lv_obj_align(this->screen,LV_ALIGN_TOP_LEFT,0,0);
        /* 消除默认屏幕的边距 */
        lv_obj_set_style_pad_all(this->screen,0,LV_PART_MAIN);
        lv_obj_set_style_border_width(this->screen,0,LV_PART_MAIN);
        /* 只要默认容器这个对象 */
        lv_obj_set_style_bg_opa(this->screen,LV_OPA_TRANSP,LV_PART_MAIN);
        return true;
    }

    /**
     * @brief 设置壁纸图片
     * @param ImageSrc 壁纸图片描述符
     */
    void DesktopViewModule::SetWallPaper(lv_image_dsc_t & ImageSrc)
    {
        lv_image_set_src(this->image , &ImageSrc);
    }

    /**
     * @brief  创建顶层文字标签
     * @param  text 文字内容
     * @return 成功返回 true, 基准屏幕未创建返回 false
     */
    bool DesktopViewModule::CreateTopText(const char* text)
    {
        if(this->screen == nullptr)
            return false;
        this->top_text = lv_label_create(this->screen);
        lv_label_set_text(this->top_text,text);
        lv_obj_set_size(this->top_text,LV_PCT(100),LV_SIZE_CONTENT);
        lv_obj_align(this->top_text,LV_ALIGN_TOP_MID,0,LV_PCT(10));
        lv_obj_set_style_text_align(this->top_text,LV_TEXT_ALIGN_CENTER,LV_PART_MAIN);
        lv_obj_set_style_text_font(this->top_text,DEKTOP_DEFAULT_FONT,LV_PART_MAIN);
        lv_obj_set_style_text_color(this->top_text,HomeColor::Blue,LV_PART_MAIN);   /* 青色 */
        return true;
    }

    /**
     * @brief 更新顶层文字内容
     * @param text 新文字内容 (top_text 为空时忽略)
     */
    void DesktopViewModule::SetTopText(const char* text)
    {
        if(this->top_text != nullptr)
            lv_label_set_text(this->top_text,text);
    }
}
