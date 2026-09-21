/**
 * @file ui_app.cpp
 * @author H-000-H
 * @brief UI 组合根实现: 输入设备装配 + 外壳创建 + 页表注册与返回栈路由
 * @note  接口说明见 ui_app.hpp, 本文件只留实现要点。
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#include "ui_app.hpp"
#include "input_backend.hpp"
#include "system_log.h"
#include <cstdint>
#include <cstring>
#include <lvgl/core/lv_group.h>

namespace ui
{
    constexpr const char* const k_tag = "ui_app";

    namespace
    {
        /** @brief 栈里没找到时的下标 */
        constexpr std::size_t k_no_slot = static_cast<std::size_t>(-1);
    } // namespace

    bool App::init(lv_display_t* disp, InputBackend& input)
    {
        if (disp == nullptr)
        {
            MT_LOG_ERROR(k_tag, "init: disp 为空, 拒绝 (不用默认屏兜底)");
            return false;
        }
        this->disp = disp;

        if (!this->set_input(input))
        {
            return false;
        }
        if (!this->desktop.create(disp))
        {
            MT_LOG_ERROR(k_tag, "desktop create failed");
            return false;
        }
        if (!this->top_bar.create(disp))
        {
            MT_LOG_ERROR(k_tag, "top bar create failed");
            return false;
        }
        return true;
    }

    bool App::set_input(InputBackend& input)
    {
        if (this->disp == nullptr)
        {
            MT_LOG_ERROR(k_tag, "set_input: disp 还没设, 先 init");
            return false;
        }
        if (this->input == &input)
        {
            return input.attach(this->disp); /* 同一个后端重复调: attach 自己幂等 */
        }
        if (this->input != nullptr)
        {
            this->input->detach(); /* 旧的先摘干净: 两个 indev 同时活在一块屏上会互相打架 */
        }

        this->input = &input;
        if (!input.attach(this->disp))
        {
            this->input = nullptr;
            MT_LOG_ERROR(k_tag, "input attach failed");
            return false;
        }
        return true;
    }

    lv_group_t* App::get_group() const
    {
        return (this->input != nullptr) ? this->input->group() : nullptr;
    }

    /* ---------------------------------------------------------- 页表: 运行期注册 */

    bool App::bind_page(Page& page)
    {
        const char* name = page.name();
        if ((name == nullptr) || (name[0] == '\0'))
        {
            MT_LOG_ERROR(k_tag, "bind_page: 页面没报名字, 拒绑");
            return false;
        }

        std::size_t free_slot = k_no_slot;
        for (std::size_t i = 0U; i < k_page_slot_max; ++i)
        {
            Page* bound = this->table[i];
            if (bound == nullptr)
            {
                if (free_slot == k_no_slot)
                {
                    free_slot = i; /* 记下第一个空槽 */
                }
                continue;
            }
            if (bound == &page)
            {
                return true; /* 重复绑同一个对象: 幂等 */
            }
            if (std::strcmp(bound->name(), name) == 0)
            {
                MT_LOG_ERROR(k_tag, "bind_page: 名字 \"%s\" 已被占用, 后一个页面进不了表", name);
                return false;
            }
        }

        if (free_slot == k_no_slot)
        {
            MT_LOG_ERROR(k_tag, "bind_page: 页表满了(%u), \"%s\" 进不去 (加大 k_page_slot_max)",
                         static_cast<unsigned>(k_page_slot_max), name);
            return false;
        }
        this->table[free_slot] = &page;
        MT_LOG_INFO(k_tag, "page \"%s\" bound (quit=%s)", name,
                    (page.quit_mode() == PageQuit::HIDE) ? "hide" : "destroy");
        return true;
    }

    Page* App::page_of(const char* name) const
    {
        if ((name == nullptr) || (name[0] == '\0'))
        {
            return nullptr;
        }
        for (std::size_t i = 0U; i < k_page_slot_max; ++i)
        {
            Page* bound = this->table[i];
            if ((bound != nullptr) && (std::strcmp(bound->name(), name) == 0))
            {
                return bound;
            }
        }
        return nullptr;
    }

    /* ---------------------------------------------------------- 换页请求: 只排队 */

    bool App::show_page(const char* name)
    {
        Page* target = this->page_of(name);
        if (target == nullptr)
        {
            MT_LOG_ERROR(k_tag, "show_page: \"%s\" 没注册", (name != nullptr) ? name : "(null)");
            return false;
        }
        if (target == this->current)
        {
            return true; /* 重复进当前页: 忽略, 不算失败 */
        }
        return this->push_route(RouteOp::SHOW, target);
    }

    bool App::back()
    {
        if (this->current == nullptr)
        {
            return false; /* 本来就没页面, 回无可回 */
        }
        return this->push_route(RouteOp::BACK, nullptr);
    }

    bool App::reset_to(const char* name)
    {
        Page* target = this->page_of(name);
        if (target == nullptr)
        {
            MT_LOG_ERROR(k_tag, "reset_to: \"%s\" 没注册", (name != nullptr) ? name : "(null)");
            return false;
        }
        return this->push_route(RouteOp::RESET, target);
    }

    bool App::push_route(RouteOp op, Page* target)
    {
        if (this->pending.op != RouteOp::NONE)
        {
            MT_LOG_WARN(k_tag, "route override: 待执行的请求被顶掉, target=\"%s\"",
                        (this->pending.target != nullptr) ? this->pending.target->name() : "(back)");
        }
        this->pending.op     = op;
        this->pending.target = target;
        return true;
    }

    /* ---------------------------------------------------------- 每帧驱动 */

    void App::tick()
    {
        /* 先执行攒下的换页请求: 它们来自上一帧的事件回调/enter/exit, 那时删控件树不安全 */
        this->flush_route();

        Page* page = this->current;
        if (page == nullptr)
        {
            return;
        }

        page->tick();

        /* 页面自己宣告完成 → 由这里收尾(回上一页), 而不是页面在事件回调里删自己 */
        if (page->is_finished())
        {
            MT_LOG_INFO(k_tag, "page \"%s\" finished", page->name());
            this->back();
            this->flush_route();
        }
    }

    void App::flush_route()
    {
        for (std::size_t hop = 0U; this->pending.op != RouteOp::NONE; ++hop)
        {
            if (hop >= k_route_chain_max)
            {
                MT_LOG_ERROR(k_tag, "route chain over %u hops, pending dropped (页面在 enter/exit 里互相跳?)",
                             static_cast<unsigned>(k_route_chain_max));
                this->pending = RouteRequest{};
                return;
            }

            const RouteRequest req = this->pending;
            this->pending          = RouteRequest{}; /* 先清再执行: 执行期间新提的请求才不会被就地吃掉 */
            this->apply_route(req.op, req.target);
        }
    }

    /* ---------------------------------------------------------- 换页执行 */

    void App::close_page(Page* page)
    {
        if (page == nullptr)
        {
            return;
        }
        if (page->quit_mode() == PageQuit::HIDE)
        {
            page->hide(); /* 树留着, 下次 enter 复用 */
        }
        else
        {
            page->exit();
        }
    }

    void App::close_current()
    {
        Page* old = this->current;
        if (old == nullptr)
        {
            return;
        }

        /* 先摘再收场: 页面在 hide/exit 里再发起换页也不会撞到自己 */
        this->current = nullptr;
        close_page(old);
    }

    std::size_t App::stack_index_of(const Page* target) const
    {
        for (std::size_t i = 0U; i < this->stack.size(); ++i)
        {
            if (this->stack[i] == target)
            {
                return i;
            }
        }
        return k_no_slot;
    }

    bool App::apply_route(RouteOp op, Page* target)
    {
        if (op == RouteOp::BACK)
        {
            Page* const gone = this->current;
            this->close_current();

            if ((!this->stack.empty()) && (this->stack.back() == gone))
            {
                this->stack.pop_back(); /* 弹掉自己 */
            }

            if (this->stack.empty())
            {
                MT_LOG_INFO(k_tag, "stack empty, back to shell only");
                return true;
            }

            Page* page    = this->stack.back();
            this->current = page;
            page->enter(this->desktop.get_screen());
            MT_LOG_INFO(k_tag, "back to \"%s\" (depth=%u)", page->name(), static_cast<unsigned>(this->stack.size()));
            return true;
        }

        if (target == nullptr)
        {
            MT_LOG_ERROR(k_tag, "route target 为空, 丢弃");
            return false;
        }

        /* 收场一律按各自 quit_mode (HIDE 只隐藏, 之后 show_page 还能复用那棵树):
           弹栈/清栈清的是"导航历史", 不是控件树 */
        if (op == RouteOp::RESET)
        {
            for (std::size_t i = this->stack.size(); i > 0U; --i)
            {
                if (this->stack[i - 1U] != target && this->stack[i - 1U] != this->current)
                {
                    close_page(this->stack[i - 1U]);
                }
                this->stack.pop_back();
            }
        }
        else
        {
            /* 目标已经在栈里 = 这次是"回退到它那层", 不是再压一遍 */
            const std::size_t found = this->stack_index_of(target);
            if (found != k_no_slot)
            {
                for (std::size_t i = this->stack.size(); i > (found + 1U); --i)
                {
                    if (this->stack[i - 1U] != this->current)
                    {
                        close_page(this->stack[i - 1U]);
                    }
                    this->stack.pop_back();
                }
            }
            else if (this->stack.full())
            {
                MT_LOG_ERROR(k_tag, "stack full (%u), show_page \"%s\" rejected",
                             static_cast<unsigned>(k_stack_depth), target->name());
                return false;
            }
        }

        if (this->current != target)
        {
            this->close_current();
        }

        if (this->stack.empty() || (this->stack.back() != target))
        {
            this->stack.push_back(target); /* 满了上面已挡掉, 这里不会越界 */
        }

        this->current = target;
        target->enter(this->desktop.get_screen());
        MT_LOG_INFO(k_tag, "page \"%s\" in, op=%s depth=%u", target->name(),
                    (op == RouteOp::RESET) ? "reset" : "show", static_cast<unsigned>(this->stack.size()));
        return true;
    }

    /* ---------------------------------------------------------- 返回键 */

    void App::bind_back_key(lv_obj_t* obj)
    {
        if (obj != nullptr)
        {
            lv_obj_add_event_cb(obj, App::back_key_cb, LV_EVENT_KEY, this);
        }
    }

    void App::back_key_cb(lv_event_t* e)
    {
        App*                 self = static_cast<App*>(lv_event_get_user_data(e));
        const std::uint32_t* key  = static_cast<const std::uint32_t*>(lv_event_get_param(e));
        if ((self != nullptr) && (key != nullptr) && (*key == LV_KEY_ESC))
        {
            self->back(); /* 只排队: 正在派发事件的这个控件此刻还不能被删 */
        }
    }
}
