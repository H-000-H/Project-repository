/**
 * @file top_bar.hpp
 * @author H-000-H
 * @brief 顶栏 (常驻外壳): 屏顶一条状态栏, 左段弹性 / 右段贴边, 项按名字增删改
 * @note   由 App 持有; 项表用 etl::vector 按值存(定容, 无堆), 删项靠 erase 平移, 不留悬空节点
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "etl/string.h"
#include "etl/vector.h"
#include "lvgl/lvgl.h"
#include <cstddef>
#include <cstdint>

namespace ui
{
    class TopBar
    {
    public:
        static constexpr std::size_t k_max_items     = 10U; /**< 项数上限 (定容, 无堆) */
        static constexpr std::size_t k_item_name_len = 16U; /**< 项名长度上限, 超长拒绝不截断 */

        TopBar()  = default;
        ~TopBar() = default;

        TopBar(const TopBar&)            = delete;
        TopBar& operator=(const TopBar&) = delete;
        TopBar(TopBar&&)                 = delete;
        TopBar& operator=(TopBar&&)      = delete;

        /**
         * @brief 建条本体 + 左右两段 flex 行 (幂等)
         * @param[in] disp lv_display_t* 本条所属的 display
         * @return bool disp 为空或 LVGL 分配失败返回 false
         */
        bool create(lv_display_t* disp);

        /**
         * @brief 加一项 (label), 项名同时是后续查找/删除的 key
         * @param[in] name    const char*   项名 (内部拷一份, 不吊在调用方指针上)
         * @param[in] width   std::int32_t  >0 固定像素(放不下打点省略, 文字居中); 0 = 按内容自适应
         * @param[in] is_head bool          true = 放左段; false = 放右段
         * @return bool 名字为空/过长、表满、条未建或分配失败返回 false
         */
        bool add_item(const char* name, std::int32_t width = 0, bool is_head = false);

        /**
         * @brief 按名字移除项 (同名多项一并移除)
         * @param[in] name const char* 项名
         * @return bool 至少移除一项返回 true
         */
        bool remove_item(const char* name);

        /**
         * @brief 设置某项文字颜色
         * @param[in] name  const char* 项名
         * @param[in] color lv_color_t  颜色
         * @return bool 找不到项返回 false
         */
        bool set_item_color(const char* name, lv_color_t color);

        /**
         * @brief 设置某项不透明度
         * @param[in] name const char* 项名
         * @param[in] opa  lv_opa_t    不透明度
         * @return bool 找不到项返回 false
         */
        bool set_item_opa(const char* name, lv_opa_t opa);

        /**
         * @brief 设置条本体背景色
         * @param[in] color lv_color_t 颜色
         * @return bool 条未建返回 false
         */
        bool set_bar_color(lv_color_t color);

        /**
         * @brief 设置条本体不透明度
         * @param[in] opa lv_opa_t 不透明度
         * @return bool 条未建返回 false
         */
        bool set_bar_opa(lv_opa_t opa);

        /** @brief 隐藏整条顶栏; 条未建返回 false */
        bool hide();

        /** @brief 恢复显示整条顶栏; 条未建返回 false */
        bool show();

    private:
        /** @brief 顶栏单项: 名字(查找 key + label 文本源) + 控件句柄 + 段位 */
        struct Item
        {
            etl::string<k_item_name_len> name;              /**< 拥有拷贝 */
            lv_obj_t*                    obj     = nullptr; /**< label 控件 */
            bool                         is_head = false;   /**< true = 左段, false = 右段 */
        };

        using ItemList = etl::vector<Item, k_max_items>;

        /**
         * @brief 按名字找首个匹配项
         * @param[in] name const char* 项名
         * @return ItemList::iterator 找不到或 name 为空返回 items.end()
         */
        ItemList::iterator find(const char* name);

        /**
         * @brief 在指定段位建一个 label
         * @param[in] is_head bool          true = 左段
         * @param[in] text    const char*   文本
         * @param[in] width   std::int32_t  >0 固定像素, 0 = 自适应
         * @return lv_obj_t* 控件句柄; 条未建或分配失败返回 nullptr
         */
        lv_obj_t* create_item_label(bool is_head, const char* text, std::int32_t width);

        lv_obj_t* bar      = nullptr; /**< 条本体 (挂 screen 顶部) */
        lv_obj_t* head_row = nullptr; /**< 左段: flex_grow=1 吃满剩余宽度 */
        lv_obj_t* tail_row = nullptr; /**< 右段: LV_SIZE_CONTENT 被挤到最右 */
        ItemList  items;              /**< 有序项表 */
    };
}
