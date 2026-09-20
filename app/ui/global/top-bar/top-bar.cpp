/**
 * @file top-bar.cpp
 * @author H-000-H
 * @brief 
 * @copyright SPDX-License-Identifier: Apache-2.0 
 */
#include "top-bar.hpp"
#include "home.hpp"
#include "etl/string_view.h"
#include <cstdint>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/core/lv_obj_style.h>
#include <lvgl/core/lv_obj_style_gen.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_color.h>
#include <lvgl/layouts/lv_flex.h>
#include <lvgl/widgets/lv_label.h>
namespace ui 
{
    /**
     * @brief  建一个"排版本"flex 行: 去掉所有装饰, 只负责排列子项
     * @param  parent 父对象
     * @return 新建的 flex 行对象, 失败返回 nullptr
     * @note 宽度默认 LV_SIZE_CONTENT(贴内容), 谁要弹性撑开由调用方加 flex_grow (见 createbarscreen)
     */
    static lv_obj_t* make_row(lv_obj_t* parent)
    {
        lv_obj_t* row = lv_obj_create(parent);
        if(row == nullptr)
            return nullptr;
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_PCT(100));
        /*水平从左到右*/
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(row, LV_OPA_0, LV_PART_MAIN);

        /*去边框长*/
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        /*去边框距*/
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        return row;
    }

    /**
     * @brief  建条本体并排版: flex 行 —— 条 = [左段 flex_grow] + [右段]
     * @return 成功(或已建)返回 true, 失败返回 false
     */
    bool TopBarViewModule::createbarscreen()
    {
        if(this->bar_screen != nullptr)
            return true;                                            
        lv_obj_t* screen = lv_screen_active();
        if(screen == nullptr)
            return false;
        this->bar_screen = lv_obj_create(screen);
        if(this->bar_screen == nullptr)
            return false;
        /* 条高跟屏高走:*/
        lv_obj_set_size(this->bar_screen, LV_PCT(100), lv_obj_get_height(screen) / kTopBarHeightDiv);
        lv_obj_align(this->bar_screen, LV_ALIGN_TOP_LEFT, 0, 0);
   
        lv_obj_set_style_bg_opa(this->bar_screen, kDefaultTopBarOpa, LV_PART_MAIN);
        lv_obj_set_style_border_width(this->bar_screen, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(this->bar_screen, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(this->bar_screen, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(this->bar_screen, 0, LV_PART_MAIN);
        /*左侧吃右侧剩余的长度 */
        lv_obj_set_flex_flow(this->bar_screen, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(this->bar_screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        this->head_row = make_row(this->bar_screen);
        this->tail_row = make_row(this->bar_screen);
        if(this->head_row != nullptr)
            lv_obj_set_flex_grow(this->head_row, 1);   /* 弹性撑开: 占满除右段外的剩余宽度 */
        if(this->head_row == nullptr || this->tail_row == nullptr)
            return false;
        return true;
    }

    /**
     * @brief 往条上加一项(label)
     * @param name    项名: 内部拷一份, 同时作为 find/remove 的 key
     * @param width   宽度: >0 固定像素(放不下打点省略, 文字居中); 0 = 按内容自适应
     * @param is_head true = 放左段; false = 放右段
     * @return 成功返回 true; 条未建/名字非法或过长/池耗尽/LVGL 分配失败返回 false
     */
    bool TopBarViewModule::addobj(const char* name, std::int32_t width, bool is_head)
    {
        if(this->bar_screen == nullptr)
            return false;
        if(name == nullptr)
            return false;
        const etl::string_view key(name);
        if(key.size() > TopBarObjNode::kNameLen)
            return false; // Name too long: etl::string 会截断
        lv_obj_t* parent = is_head ? this->head_row : this->tail_row;
        if(parent == nullptr)
            return false; // 条还没建(createbarscreen 没成功)
        TopBarObjNode* obj_new_node = this->node_pool.create();
        if(obj_new_node == nullptr)
            return false; // Pool exhausted
        lv_obj_t* obj_new = lv_label_create(parent);
        if(obj_new == nullptr)
        {
            this->node_pool.destroy(obj_new_node); // LVGL out of memory, don't leak the node
            return false;
        }
        obj_new_node->name = key;   // 拷进节点自己那份
        /* label 的文本用节点那份: 跟着节点走, 不吊在调用方指针上 */
        lv_label_set_text(obj_new, obj_new_node->name.c_str());
        /* 宽度: 给了就用固定像素, 没给按内容自适应 一般都是直接自适应*/
        lv_obj_set_size(obj_new, (width > 0) ? width : LV_SIZE_CONTENT, LV_PCT(100));
        /*颜色默认白色*/
        lv_obj_set_style_text_color(obj_new,HomeColor::White,LV_PART_MAIN);
        
        lv_obj_set_style_text_font(obj_new,BAR_FONT_DEFAULT,LV_PART_MAIN);
        if(width > 0)
        {
            lv_label_set_long_mode(obj_new, LV_LABEL_LONG_DOT);                        // 固定宽度: 放不下就打点省略
            lv_obj_set_style_text_align(obj_new, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);  // 固定宽度时文字居中
        }
        obj_new_node->obj = obj_new;
        obj_new_node->is_head = is_head;
        this->obj_list.push_back(obj_new_node);
        return true;
    }

    /**
     * @brief  按名字移除条上的项 (可移除多个同名项)
     * @param  name 项名
     * @return 至少移除一项返回 true, 否则 false
     */
    bool TopBarViewModule::removeobj(const char* name)
    {
        if(this->bar_screen == nullptr || name == nullptr)
            return false;
        bool removed = false;
        for(auto it = this->obj_list.begin(); it != this->obj_list.end(); )
        {
            if((*it)->name == name)
            {
                lv_obj_delete((*it)->obj);   
                this->node_pool.destroy(*it); // 析构(里面是 etl::string) 再还给静态池
                it = this->obj_list.erase(it); // erase returns the next valid iterator
                removed = true;
            }
            else
            {
                ++it;
            }
        }
        return removed;   /* flex 自己会补上空位, 不用 relayout */
    }

    /**
     * @brief  按名字查找条上的项
     * @param  name 项名
     * @return 找到返回对象指针, 否则 nullptr
     */
    lv_obj_t* TopBarViewModule::findobj(const char* name)
    {
        if(this->bar_screen == nullptr || name == nullptr)
            return nullptr;
        
        for(auto it : this->obj_list)
            if(it->name == name)
                return it->obj; // Return the object if found
        return nullptr; // Return nullptr if not found
    }

    /**
     * @brief  设置某项的文字颜色
     * @param  name  项名
     * @param  color 目标颜色
     * @return 成功返回 true, 条未建或找不到项返回 false
     */
    bool TopBarViewModule::setobjcolor(const char* name, lv_color_t color)
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_t* obj = this->findobj(name);
        if(obj == nullptr)
            return false;
        lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
        return true;
    }
    /**
     * @brief  设置条本体背景色
     * @param  color 目标颜色
     * @return 成功返回 true, 条未建返回 false
     */
    bool TopBarViewModule::setscreenbgcolor(lv_color_t color)
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_set_style_bg_color(this->bar_screen, color, LV_PART_MAIN);
        return true;        
    }

    /**
     * @brief  设置条本体不透明度
     * @param  opa 目标不透明度
     * @return 成功返回 true, 条未建返回 false
     */
    bool TopBarViewModule::screensetopa(lv_opa_t opa)
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_set_style_opa(this->bar_screen, opa, LV_PART_MAIN);
        return true;
    }

    /**
     * @brief  设置某项不透明度
     * @param  name 项名
     * @param  opa  目标不透明度
     * @return 成功返回 true, 条未建或找不到项返回 false
     */
    bool TopBarViewModule::objsetopa(const char* name, lv_opa_t opa)
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_t* obj = this->findobj(name);
        if(obj == nullptr)
            return false;
        lv_obj_set_style_opa(obj, opa, LV_PART_MAIN);
        return true;
    }

    /**
     * @brief  隐藏整条顶栏
     * @return 成功返回 true, 条未建返回 false
     */
    bool TopBarViewModule::hide_opa_all_topbar()
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_set_hidden(this->bar_screen, true);
        return true;
    }

    /**
     * @brief  恢复显示整条顶栏
     * @return 成功返回 true, 条未建返回 false
     */
    bool TopBarViewModule::restore_opa_all_topbar()
    {
        if(this->bar_screen == nullptr)
            return false;
        lv_obj_set_hidden(this->bar_screen, false);
        return true;
    }
}
