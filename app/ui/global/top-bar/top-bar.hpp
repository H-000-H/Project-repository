/**
 * @file top-bar.hpp
 * @author H-000-H
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef TOP_BAR_HPP
#define TOP_BAR_HPP
#include "lvgl/lvgl.h"
#include "etl/vector.h"
#include "etl/pool.h"
#include "etl/string.h"
#include <cstddef>
#include <cstdint>
#include <lvgl/config/lv_conf_internal.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/draw/lv_color.h>
namespace ui 
{
    /* 条底默认全透明: 让下面的壁纸透出来, 只留子项(文字/图标) —— 想磨砂半透明就改回 LV_OPA_70 */
    constexpr const lv_opa_t kDefaultTopBarOpa = LV_OPA_TRANSP;
    constexpr std::int32_t kTopBarHeightDiv = 10;/*bar的高度*/
    #define   BAR_FONT_DEFAULT LV_FONT_DEFAULT_MONTSERRAT_12
struct TopBarObjNode
{
public:
    static constexpr std::size_t kNameLen = 16;
    etl::string<kNameLen> name; // Owned copy, caller string may be temporary
    lv_obj_t* obj;
    bool is_head;
};

class TopBarViewModule
{
public:
    TopBarViewModule() = default;  /**< 默认构造 */
    ~TopBarViewModule() = default; /**< 默认析构 */
    lv_obj_t* bar_screen = nullptr;              // 条本体(挂 screen 顶部)
    etl::vector<TopBarObjNode*, 10> obj_list;   // 已加项的节点(名字→obj)
    etl::pool<TopBarObjNode, 10> node_pool;     // Static node pool, no heap allocation

    /**
     * @brief  建条本体并排版: flex 行 —— 条 = [左段 flex_grow] + [右段], 两段各自是 flex 行
     * @return 成功(或已建)返回 true, 失败返回 false
     */
    bool createbarscreen();
    /**
     * @brief 往条上加一项(label)
     * @param name   项名: 内部拷一份, 同时作为 find/remove 的 key
     * @param width  宽度: >0 固定像素(放不下用 LV_LABEL_LONG_DOT 打点省略, 文字居中);
     *               0(默认) = LV_SIZE_CONTENT —— label 就是文字宽度
     * @param is_head true = 放左段; false = 放右段
     * @note 高度不用给: 子项一律跟条高 (LV_PCT(100)); 位置交给条的 flex 排
     * @return 成功返回 true; 条未建/名字非法或过长/池耗尽/LVGL 分配失败返回 false
     */
    bool addobj(const char* name, std::int32_t width = 0, bool is_head = false);
    /**
     * @brief  按名字移除条上的项 (可移除多个同名项)
     * @param  name 项名
     * @return 至少移除一项返回 true, 否则 false
     */
    bool removeobj(const char* name);
    /**
     * @brief  按名字查找条上的项
     * @param  name 项名
     * @return 找到返回对象指针, 否则 nullptr
     */
    lv_obj_t* findobj(const char* name);
    /**
     * @brief  设置某项的文字颜色
     * @param  name  项名
     * @param  color 目标颜色
     * @return 成功返回 true, 条未建或找不到项返回 false
     */
    bool setobjcolor(const char* name, lv_color_t color);
    /**
     * @brief  设置条本体背景色
     * @param  color 目标颜色
     * @return 成功返回 true, 条未建返回 false
     */
    bool setscreenbgcolor(lv_color_t color);
    /**
     * @brief  设置条本体不透明度
     * @param  opa 目标不透明度
     * @return 成功返回 true, 条未建返回 false
     */
    bool screensetopa(lv_opa_t opa);
    /**
     * @brief  设置某项不透明度
     * @param  name 项名
     * @param  opa  目标不透明度
     * @return 成功返回 true, 条未建或找不到项返回 false
     */
    bool objsetopa(const char* name, lv_opa_t opa);
    /**
     * @brief  隐藏整条顶栏
     * @return 成功返回 true, 条未建返回 false
     */
    bool hide_opa_all_topbar();
    /**
     * @brief  恢复显示整条顶栏
     * @return 成功返回 true, 条未建返回 false
     */
    bool restore_opa_all_topbar();
protected:
    lv_obj_t* head_row = nullptr;   // 左段: flex 行, flex_grow=1 弹性撑开(吃满剩余宽度)
    lv_obj_t* tail_row = nullptr;   // 右段: flex 行, LV_SIZE_CONTENT —— 被左段挤到最右
};

class TopBarDataModule
{
public:
    TopBarDataModule() = default;  /**< 默认构造 */
    ~TopBarDataModule() = default; /**< 默认析构 */
};

class TopBarControllerModule : public TopBarViewModule, public TopBarDataModule
{
public:
    TopBarControllerModule() = default;  /**< 默认构造 */
    ~TopBarControllerModule() = default; /**< 默认析构 */
};

class TopBar :public TopBarControllerModule
{
public:
    TopBar() = default;  /**< 默认构造 */
    ~TopBar() = default; /**< 默认析构 */
    TopBar(const TopBar&) = delete;            /**< 禁用拷贝构造 */
    TopBar& operator=(const TopBar&) = delete; /**< 禁用拷贝赋值 */
    TopBar(TopBar&&) = delete;                 /**< 禁用移动构造 */
    TopBar& operator=(TopBar&&) = delete;      /**< 禁用移动赋值 */
    /**
     * @brief  获取顶栏单例
     * @return 顶栏单例引用
     */
    static TopBar& GetInstance(){static TopBar instance ; return instance;}
};
}
#endif
