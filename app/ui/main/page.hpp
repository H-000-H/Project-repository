/**
 * @file page.hpp
 * @author H-000-H
 * @brief 页面基类: 一个页面 = 一个 Page 子类; 页面自报名字, App 运行期建页表
 * @note  页面自己管自己的控件树生死; 页面之间互不认识: 想说"去哪"只能写注册名字符串。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lvgl/lvgl.h"
#include <cstdint>

namespace ui
{
    /** @brief 一级页面默认宽度 (每个页面类可以自己调) */
    constexpr std::int32_t k_page_default_width = 200;
    /** @brief 一级页面默认高度 (每个页面类可以自己调) */
    constexpr std::int32_t k_page_default_height = 160;

    /** @brief 离开页面时怎么收控件树; 判据: 这次回来要不要重头开始 */
    enum class PageQuit : std::uint8_t
    {
        DESTROY, /**< 拆树: 一次性流程页(锁屏/向导), 再进来是全新一次 */
        HIDE     /**< 只隐藏: 留住界面状态(输入草稿/滚动位置/选中的项) */
    };

    class Page
    {
    public:
        /**
         * @brief 构造
         * @param[in] quit PageQuit    默认收场方式
         * @param[in] name const char* 注册名 (字面量常驻, 只存指针)
         */
        Page(PageQuit quit, const char* name) : quit_on_exit(quit), page_name(name) {}
        virtual ~Page() = default;

        Page(const Page&)            = delete;
        Page& operator=(const Page&) = delete;
        Page(Page&&)                 = delete;
        Page& operator=(Page&&)      = delete;

        /**
         * @brief 进入页面: 建控件树(或复用隐藏中的那棵) + 抢焦点
         * @param[in] parent lv_obj_t* 挂载父对象, 由 App 传入桌面场景根
         * @note  必须幂等: 已经在屏上时重复 enter 不能重复建控件
         */
        virtual void enter(lv_obj_t* parent) = 0;

        /** @brief 每帧驱动 (没有周期动作的页面不用重写) */
        virtual void tick() {}

        /** @brief 退出页面: 拆控件树 + 复原外壳 (拆完还能再次 enter) */
        virtual void exit() = 0;

        /**
         * @brief 页面宣告"我这一屏办完了"
         * @return bool true = App 在本帧 tick 末尾收掉本页并回上一页
         * @note  页面自己不删自己、也不自己跳页 —— 否则会在事件回调里销毁正在回调的对象
         */
        virtual bool is_finished() const = 0;

        /** @brief 注册名 (日志和查表都用它) */
        const char* name() const { return this->page_name; }

        /** @brief 离开时的收场方式 */
        PageQuit quit_mode() const { return this->quit_on_exit; }

        /**
         * @brief 改收场方式 (运行期随时可换)
         * @param[in] quit PageQuit 新的收场方式
         * @note  只能在 UI 线程、App::tick 的换页执行里或页面自己的 enter/tick 里调;
         *        别在 hide/exit 执行途中改 —— App 正按旧值分派
         */
        void set_quit_mode(PageQuit quit) { this->quit_on_exit = quit; }

        /**
         * @brief 设置页面宽度 (按屏宽百分比)
         * @param[in] self  lv_obj_t*   页面根对象
         * @param[in] width std::int32_t 宽度百分比 (0~100)
         */
        void set_page_width(lv_obj_t* self, std::int32_t width) { lv_obj_set_width(self, LV_PCT(width)); }

        /**
         * @brief 设置页面高度 (按屏高百分比)
         * @param[in] self   lv_obj_t*   页面根对象
         * @param[in] height std::int32_t 高度百分比 (0~100)
         */
        void set_page_height(lv_obj_t* self, std::int32_t height) { lv_obj_set_height(self, LV_PCT(height)); }

        /**
         * @brief 隐藏本页: HIDE 页的收场动作 —— 只把根对象藏起来, 树和数据都留着
         * @note  页面上有焦点控件时必须重写补一句 lv_group_remove_obj, 否则按键会打到看不见的对象上
         */
        virtual void hide()
        {
            if (this->root != nullptr)
            {
                lv_obj_set_hidden(this->root, true);
            }
        }

        /** @brief 控件树还在不在(隐藏着也算在) */
        bool has_widgets() const { return this->root != nullptr; }

    protected:
        /**
         * @brief 登记根对象 (之后隐藏/复用/拆除都认它)
         * @param[in] root lv_obj_t* 本页控件树根
         */
        void set_root(lv_obj_t* root) { this->root = root; }

        /** @brief 根对象; 没建时 nullptr */
        lv_obj_t* get_root() const { return this->root; }

        /**
         * @brief 复用隐藏中的树: 取消隐藏
         * @return bool true = 树原本藏着的, 现已亮出来; false = 树不在, 或本来就在屏上
         */
        bool reuse_root()
        {
            if ((this->root == nullptr) || !lv_obj_is_hidden(this->root))
            {
                return false;
            }
            lv_obj_set_hidden(this->root, false);
            return true;
        }

        /** @brief 拆掉根对象并清登记 (派生类删树只能走这里, 直接 lv_obj_delete 会让 root 悬空) */
        void destroy_root()
        {
            if (this->root != nullptr)
            {
                lv_obj_delete(this->root);
                this->root = nullptr;
            }
        }

    private:
        PageQuit    quit_on_exit;   /**< 收场方式, 运行期可改 (set_quit_mode) */
        const char* page_name;      /**< 注册名 (字符串字面量, 常驻) */
        lv_obj_t*   root = nullptr; /**< 本页控件树根 */
    };
}
