/**
 * @file ui_app.hpp
 * @author H-000-H
 * @brief 一块屏的 UI 会话: 页表(运行期注册) + 返回栈 + 常驻外壳(桌面/顶栏) + 输入后端 + 每帧驱动
 * @note  "一块屏一个 App": 创建时必须拿到本屏 display, 不问全局要屏; indev 也不由本类建
 *        (不绑 display 就只作用在默认屏), 交给输入后端一起管。外壳只建一次。
 * @note  换页必须排队到 tick: 跳页动机常来自 LVGL 事件回调, 在回调里当场删控件树等于删掉
 *        LVGL 正在派发的那个对象 —— 崩溃。所以三个换页接口只写 pending, 到 tick 才动手。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef UI_APP_HPP
#define UI_APP_HPP
#include "dektop.hpp"
#include "page.hpp"
#include "top_bar.hpp"
#include <cstddef>
#include <cstdint>
#include <etl/array.h>
#include <etl/vector.h>

namespace ui
{
    // 输入后端(按键/指针): 这里只存指针, 定义见 indev/input_backend.hpp
    class InputBackend;

    // 一块屏的 UI 会话
    class App
    {
    public:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        static constexpr std::size_t k_page_slot_max   = 8U; // 页表槽数上限 (静态内存)
        static constexpr std::size_t k_stack_depth     = 4U; // 返回栈最多几层 (含栈底)
        static constexpr std::size_t k_route_chain_max = 4U; // 一次 tick 最多连着换几页

        App()  = default;
        ~App() = default;

        App(const App&)            = delete;
        App& operator=(const App&) = delete;
        App(App&&)                 = delete;
        App& operator=(App&&)      = delete;

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 装输入后端 + 建桌面 + 建顶栏 (必须在 LVGL 初始化、display 建好之后调)
         * @param[in] disp  lv_display_t* 本会话服务的 display (不能为空)
         * @param[in] input InputBackend& 输入后端, 本类不接管所有权
         * @return bool 全部就绪返回 true
         */
        bool init(lv_display_t* disp, InputBackend& input);

        /**
         * @brief 换输入后端 (按键 ↔ 鼠标/触屏): 旧的 detach, 新的 attach 到本屏
         * @param[in] input InputBackend& 新后端
         * @return bool 新后端就绪返回 true
         */
        bool set_input(InputBackend& input);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 注册一个页面到页表 (名字取 page.name())
         * @param[in] page Page& 页面对象 (须长生命周期, 本类只存指针)
         * @return bool 名字为空 / 名字重复 / 槽位满 返回 false
         */
        bool bind_page(Page& page);

        /**
         * @brief 前进到该名字的页 (当前页按 quit_mode 收场, 目标压栈)
         * @param[in] name const char* 页面注册名
         * @return bool 没注册返回 false
         */
        bool show_page(const char* name);

        /**
         * @brief 回上一页; 栈里没别人时就地收掉, 只剩外壳
         * @return bool 本来就没页面返回 false
         */
        bool back();

        /**
         * @brief 清栈重开: 只清导航历史, 目标页当栈底
         * @param[in] name const char* 页面注册名
         * @return bool 没注册返回 false
         */
        bool reset_to(const char* name);

        // 每帧: 先执行攒下的换页请求, 再驱动当前页并收尾"已办完"的页
        void tick();

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 当前页注册名; 只剩外壳时为 "(none)"
        const char* current_page_name() const { return (this->current != nullptr) ? this->current->name() : "(none)"; }

        // 返回栈里现在几层(含当前页); 0 = 只剩外壳
        std::size_t stack_depth() const { return this->stack.size(); }

        // 桌面外壳 (页面挂控件树 / 改提示文字用)
        Desktop& get_desktop() { return this->desktop; }

        // 顶栏外壳 (页面改状态栏项用)
        TopBar& get_top_bar() { return this->top_bar; }

        // 本会话的 display
        lv_display_t* display() const { return this->disp; }

        // 输入后端 (换模式/查状态用), 未装时为 nullptr
        InputBackend* get_input() const { return this->input; }

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 焦点组: 页面 enter 时把自己的输入控件加进来
         * @return lv_group_t* 焦点组; 指针后端返回 nullptr, 调用方必须容忍
         */
        lv_group_t* get_group() const;

        /**
         * @brief 给一个可聚焦控件加"返回"处理: 收到 LV_KEY_ESC 就走 back()
         * @param[in] obj lv_obj_t* 能吃按键的控件 (通常是焦点入口)
         * @note  LVGL 的 keypad 只把键值发给当前焦点对象, 不向父级冒泡, 所以只能逐个控件绑
         */
        void bind_back_key(lv_obj_t* obj);

    private:
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        // 换页动作: 只在 App::tick 里被执行
        enum class RouteOp : std::uint8_t
        {
            NONE = 0U,
            SHOW,
            BACK,
            RESET
        };

        // 一条换页请求 (只有一个槽位: 后到的覆盖先到的, 覆盖时打日志)
        struct RouteRequest
        {
            RouteOp op     = RouteOp::NONE;
            Page*   target = nullptr;
        };

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        /**
         * @brief 按注册名查页表
         * @param[in] name const char* 注册名
         * @return Page* 命中的页面; 没注册返回 nullptr (不打日志, 由调用方打)
         */
        Page* page_of(const char* name) const;

        /**
         * @brief 排队一条换页请求
         * @param[in] op     RouteOp 动作
         * @param[in] target Page*   目标页 (BACK 传 nullptr)
         * @return bool 恒 true
         */
        bool push_route(RouteOp op, Page* target);

        /**
         * @brief 执行一条换页请求 (全程序唯一改 stack/current 的地方)
         * @param[in] op     RouteOp 动作
         * @param[in] target Page*   目标页
         * @return bool 成功返回 true
         */
        bool apply_route(RouteOp op, Page* target);

        // 把攒下的请求按链执行掉, 最多 k_route_chain_max 跳
        void flush_route();

        // 收当前页: 按它自己的 quit_mode 决定隐藏还是拆树 (先摘 current 再收场)
        void close_current();

        /**
         * @brief 收某一页(不碰栈): 按各自 quit_mode 隐藏或拆树
         * @param[in] page Page* 要收的页
         * @note  全程序收场只有这一个动作 —— 隐藏中的树随时能被 show_page 复用
         */
        static void close_page(Page* page);

        /**
         * @brief 目标页在栈里的位置
         * @param[in] target const Page* 目标页
         * @return std::size_t 栈下标; 不在栈里返回 k_no_slot
         */
        std::size_t stack_index_of(const Page* target) const;

        /**
         * @brief LVGL 事件回调适配 (user_data = this): 键值 == LV_KEY_ESC 就 back()
         * @param[in] e lv_event_t* LVGL 事件
         */
        static void back_key_cb(lv_event_t* e);

/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------------------------------------------------------------ */
        Desktop       desktop;         // 常驻: 壁纸 + 场景根 + 提示文字
        TopBar        top_bar;         // 常驻: 顶部状态栏
        lv_display_t* disp  = nullptr; // 本会话的 display
        InputBackend* input = nullptr; // 输入后端 (非拥有指针)

        etl::array<Page*, k_page_slot_max> table{};           // 页表: 存指针, 名字问 page->name()
        etl::vector<Page*, k_stack_depth>  stack{};           // 返回栈: 栈底是首页
        Page*                              current = nullptr; // 当前页 (非拥有指针)
        RouteRequest                       pending{};         // 攒着待执行的换页请求
    };
}

#endif // UI_APP_HPP
