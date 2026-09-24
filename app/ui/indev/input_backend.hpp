/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file input_backend.hpp
 * @author H-000-H
 * @brief 输入后端: 按键 / 指针(鼠标·触屏) 各一套, "建 indev + 绑哪块屏 + 要不要焦点组"绑成一个整体
 * @note  为什么要有这一层: lv_indev_create() 会把"创建那一刻的默认 display"记进 indev->disp,
 *        不显式 lv_indev_set_display() 就永远只作用在第一块屏上 —— 所以"谁建 indev、谁绑
 *        display、谁删"必须由同一个对象负责。按键要焦点组, 指针不要(点谁谁得焦点)。
 * @note  按键这条链跨两个线程, 别挪: 按键线程 open() 一次 / 事件回调写队列 / sample() 每周期;
 *        UI 线程 attach/detach/bind + LVGL read 回调 step() 读队列。绑定表 event_num[] 是
 *        "UI 线程写、按键线程读", 靠 release/acquire 发布条数, 先写数据再发布。
 */
#ifndef INPUT_BACKEND_HPP
#define INPUT_BACKEND_HPP
#include "app_button_base.hpp"
#include "buffer.h"
#include "compiler_compat.h"
#include "lib/button.hpp"
#include "lvgl.h"
#include "status.h"
#include "system_log.h"
#include <cstdint>
#include <etl/optional.h>

namespace ui
{
    /** @brief 本文件日志 tag */
    inline constexpr const char* k_tag_input = "input";

    /** @brief 输入后端基类: 一块屏用哪套输入由它决定; attach → 用 → (可选)detach → 再 attach */
    class InputBackend
    {
    public:
        InputBackend()          = default;
        virtual ~InputBackend() = default;

        InputBackend(const InputBackend&)            = delete;
        InputBackend& operator=(const InputBackend&) = delete;
        InputBackend(InputBackend&&)                 = delete;
        InputBackend& operator=(InputBackend&&)      = delete;

        /**
         * @brief 建 indev 并绑到指定屏 (幂等: 已就绪直接返回 true)
         * @param[in] disp lv_display_t* 本后端服务的 display
         * @return bool disp 为空或 LVGL 分配失败返回 false
         * @note  disp 为空直接拒: LVGL 允许 indev->disp = NULL(退回默认屏), 但那就等于没绑屏
         */
        virtual bool attach(lv_display_t* disp) = 0;

        /** @brief 解绑并删掉 indev/group (换输入模式用); 没建过是空操作 */
        virtual void detach() = 0;

        /** @brief 焦点组句柄; 没有"焦点"概念的后端(鼠标/触屏)返回 nullptr */
        virtual lv_group_t* group() = 0;

        /** @brief 是否已就绪 (attach 成功过且没 detach) */
        virtual bool ready() const = 0;
    };

    /**
     * @brief 按键后端: keypad indev + 焦点组 + 按键桥接(事件→LVGL 键值队列)
     * @note  默认绑法: 短按 LV_KEY_ENTER, 长按 LV_KEY_ESC(返回, 页面用 App::bind_back_key 接)
     */
    class KeypadInput final : public InputBackend
    {
    public:
        static constexpr std::uint8_t k_event_key_max = 2U; /**< 单事件最多绑几个键 (连发上限) */
        /** @brief 事件槽数: 枚举从 1 起、表从 0 起, 末尾 k_pressed_none 是哨兵不进表 */
        static constexpr std::uint8_t k_event_num =
            static_cast<std::uint8_t>(button::Button_Event::kPressed_Event_Num) - 2U;
        static constexpr std::uint16_t k_queue_len = 8U; /**< 键值队列长度 (2 的幂: 无锁 SPSC 环) */

        static_assert((k_queue_len & (k_queue_len - 1U)) == 0U, "队列长度必须是 2 的幂");

        /**
         * @brief 构造
         * @param[in] button_label const char* 按键的 DTS label (open 时按它找设备)
         */
        explicit KeypadInput(const char* button_label = "button2") : label_(button_label) {}

        /* ---------------- UI 线程 ---------------- */

        bool attach(lv_display_t* disp) override
        {
            if (disp == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "keypad: disp 为空, 拒绝 (空会退回默认屏)");
                return false;
            }
            if (this->indev_ != nullptr)
            {
                return true; /* 幂等 */
            }

            this->indev_ = lv_indev_create();
            if (this->indev_ == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "keypad: indev create failed");
                return false;
            }
            lv_indev_set_type(this->indev_, LV_INDEV_TYPE_KEYPAD);
            lv_indev_set_read_cb(this->indev_, KeypadInput::read_trampoline);
            lv_indev_set_user_data(this->indev_, this);
            lv_indev_set_display(this->indev_, disp); /* 关键: 不绑就只作用在默认屏 */

            this->group_ = lv_group_create();
            if (this->group_ == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "keypad: group create failed");
                lv_indev_delete(this->indev_);
                this->indev_ = nullptr;
                return false;
            }
            lv_group_set_wrap(this->group_, true); /* 开焦点循环 */
            lv_indev_set_group(this->indev_, this->group_);

            /* 默认绑法: 短按 ENTER, 长按 ESC(返回) */
            if (!this->bind(button::Button_Event::kPressed_Short_Over, LV_KEY_ENTER))
            {
                MT_LOG_WARN(k_tag_input, "keypad: 短按绑 LV_KEY_ENTER 失败");
            }
            if (!this->bind(button::Button_Event::kPressed_Long_Over, LV_KEY_ESC))
            {
                MT_LOG_WARN(k_tag_input, "keypad: 长按绑 LV_KEY_ESC 失败");
            }

            MT_LOG_INFO(k_tag_input, "keypad attached (button=%s)", (this->label_ != nullptr) ? this->label_ : "(null)");
            return true;
        }

        void detach() override
        {
            if (this->indev_ != nullptr)
            {
                /* 先删 indev(连带它的 read timer), 再删组: lv_indev_delete 不释放 group */
                lv_indev_delete(this->indev_);
                this->indev_ = nullptr;
            }
            if (this->group_ != nullptr)
            {
                /* group 里的控件不受影响: lv_group_delete 只把各控件的 group_p 置空, 不删控件 */
                lv_group_delete(this->group_);
                this->group_ = nullptr;
            }
        }

        lv_group_t* group() override { return this->group_; }
        bool        ready() const override { return this->indev_ != nullptr; }

        /**
         * @brief 绑一条: 某个按键事件 → 一个 LVGL 键值 (同一事件调多次 = 连发)
         * @param[in] event button::Button_Event 要绑的按键事件
         * @param[in] key   lv_key_t            对应的 LVGL 键值 (0 = 不绑)
         * @return bool 事件非法 / 该事件已绑满 k_event_key_max 条 / 短按表示冲突 返回 false
         * @note  短按有两种表示: 松手即发的 kPressed_Short_Over, 与 500ms 后结算的 Click 系列;
         *        同一次短按会先后都发, 所以这两类之间只允许占一边, 混绑直接拒
         */
        bool bind(button::Button_Event event, lv_key_t key)
        {
            const std::uint8_t idx = static_cast<std::uint8_t>(event) - 1U;
            if ((key == 0U) || (idx >= k_event_num) ||
                (MINI_ATOMIC_LOAD(&this->data_.event_num[idx], MINI_RELAXED) >= k_event_key_max))
            {
                return false;
            }

            const bool bind_short_over = (event == button::Button_Event::kPressed_Short_Over);
            const bool bind_click      = is_click_event(event);
            if ((bind_short_over && has_click_bind()) ||
                (bind_click && has_bind(button::Button_Event::kPressed_Short_Over)))
            {
                return false;
            }

            /* 写者只有 UI 线程: 先写数据, 再 release 发布条数 */
            const std::uint8_t num      = MINI_ATOMIC_LOAD(&this->data_.event_num[idx], MINI_RELAXED);
            this->data_.event[idx][num] = key;
            MINI_ATOMIC_STORE(&this->data_.event_num[idx], static_cast<std::uint8_t>(num + 1U), MINI_RELEASE);
            return true;
        }

        /** @brief 清掉全部绑定 (换页面/换模式: 先清再重绑) */
        void clear_bind()
        {
            for (std::uint8_t i = 0U; i < k_event_num; ++i)
            {
                MINI_ATOMIC_STORE(&this->data_.event_num[i], 0U, MINI_RELEASE);
            }
        }

        /* ---------------- 按键线程 ---------------- */

        /** @brief 打开按键设备并注册事件回调 (按键线程起来时调一次); 不建任何 LVGL 对象 */
        void open()
        {
            if (this->button_.has_value())
            {
                return;
            }
            this->button_.emplace(this->label_, this->data_);
            this->button_->register_callback();
        }

        /** @brief 采样本颗按键电平 (按键线程每周期一次, 之后统一 button::Button::scan 推进状态机) */
        void sample()
        {
            if (this->button_.has_value())
            {
                this->button_->sample_level();
            }
        }

        /**
         * @brief 按键线程钩子适配: 打开桥接
         * @param[in] ctx void* KeypadInput 实例
         */
        static void open_thunk(void* ctx)
        {
            if (ctx != nullptr)
            {
                static_cast<KeypadInput*>(ctx)->open();
            }
        }

        /**
         * @brief 按键线程钩子适配: 采样
         * @param[in] ctx void* KeypadInput 实例
         */
        static void sample_thunk(void* ctx)
        {
            if (ctx != nullptr)
            {
                static_cast<KeypadInput*>(ctx)->sample();
            }
        }

    private:
        /**
         * @brief 按键线程与 UI 线程之间的桥接数据
         * @note  队列只由按键线程写、UI 线程读 (SPSC); 绑定表 UI 线程写、按键线程读
         */
        struct BridgeData
        {
            /* 事件 → 键值序列: 一个事件可绑多个键, 触发后按绑定顺序逐个入队 */
            lv_key_t event[k_event_num][k_event_key_max] = {};
            /* 条数跨线程发布用: 先写数据, 再 release 发布条数 */
            MINI_ATOMIC_UINT8 event_num[k_event_num] = {};

            /* fifo_spsc 内部是 uintptr_t, 32/64 位平台都能正确存 lv_key_t */
            struct fifo_spsc queue                  = {};
            fifo_data_type   queue_buf[k_queue_len] = {};
            std::uint16_t    drop                   = 0U; /**< 队列满被丢的键数 */
        };

        /** @brief 本颗按键: 把按键库事件翻译成 LVGL 键值入队 (只依赖 BridgeData) */
        class LvglButton final : public app::AppButton<BridgeData>
        {
        public:
            /**
             * @brief 构造
             * @param[in] label const char* 按键 DTS label
             * @param[in] data  BridgeData& 本按键的桥接数据 (生命周期须长于本对象)
             */
            LvglButton(const char* label, BridgeData& data)
                : app::AppButton<BridgeData>(label, app::k_trigger_level_low, data)
            {
            }

        private:
            /**
             * @brief 按键库事件回调 —— 跑在按键线程, 只做入队
             * @param[in] self button::Button& 触发事件的按钮库对象
             * @param[in] data BridgeData&     本按键的桥接数据
             * @return bool 恒 true
             */
            bool button_callback(button::Button& self, BridgeData& data) override final
            {
                /* 枚举从 1 起, 表从 0 起; k_pressed_none 已被 -2 排除在表外 */
                const std::uint8_t idx = static_cast<std::uint8_t>(self.event_read()) - 1U;
                if (idx >= k_event_num)
                {
                    return false;
                }

                /* acquire 读条数 → 保证读到 num 时前 num 条数据已写完 (UI 线程正在写绑定表) */
                const std::uint8_t num = MINI_ATOMIC_LOAD(&data.event_num[idx], MINI_ACQUIRE);
                for (std::uint8_t i = 0U; i < num; ++i)
                {
                    const fifo_data_type key = static_cast<fifo_data_type>(data.event[idx][i]);
                    if (fifo_write_data(&data.queue, key) != BUFF_OK)
                    {
                        ++data.drop; /* 满: 丢最新那个, 已入队的按顺序保留 */
                        break;
                    }
                }
                return true;
            }
        };

        /**
         * @brief LVGL read 回调适配 (user_data = this): 吐出待上报的键
         * @param[in]  indev lv_indev_t*     本输入设备
         * @param[out] data  lv_indev_data_t* 写入键值/状态
         */
        static void read_trampoline(lv_indev_t* indev, lv_indev_data_t* data)
        {
            auto* self = static_cast<KeypadInput*>(lv_indev_get_user_data(indev));
            if ((self == nullptr) || (data == nullptr))
            {
                return;
            }
            if (!self->step(*data))
            {
                data->state            = LV_INDEV_STATE_RELEASED;
                data->continue_reading = false;
            }
        }

        /**
         * @brief 一个键 = 一次 PRESSED + 一次 RELEASED (跑在 UI 线程)
         * @param[out] data lv_indev_data_t& 写入键值/状态
         * @return bool false = 没有待上报的键 (data 未写入)
         * @note  中间必须夹一次松开, 否则第二个键进不了"按下"分支(LVGL 只在 RELEASED→PRESSED 沿发 PRESSED)
         */
        bool step(lv_indev_data_t& data)
        {
            if (this->pressed_)
            {
                /* 上一步吐了按下, 这一步必须补松开 */
                this->pressed_ = false;
                data.state     = LV_INDEV_STATE_RELEASED;
                /* 队列里还有键 → 让 LVGL 在同一个周期里接着读下一个 */
                bool empty = true;
                MINI_IGNORE_RESULT(fifo_isempty(&this->data_.queue, &empty));
                data.continue_reading = !empty;
                return true;
            }

            fifo_data_type key = 0;
            if (fifo_read_data(&this->data_.queue, &key) != BUFF_OK)
            {
                return false; /* 队列空 */
            }

            data.key              = static_cast<lv_key_t>(key);
            this->pressed_        = true;
            data.state            = LV_INDEV_STATE_PRESSED;
            data.continue_reading = true;
            return true;
        }

        /**
         * @brief 该事件当前是否已绑 (至少一个键)
         * @param[in] event button::Button_Event 待查事件
         * @return bool 已绑返回 true
         */
        bool has_bind(button::Button_Event event) const
        {
            const std::uint8_t i = static_cast<std::uint8_t>(event) - 1U;
            return (i < k_event_num) && (MINI_ATOMIC_LOAD(&this->data_.event_num[i], MINI_RELAXED) != 0U);
        }

        /** @brief 是否已绑了任一结算类短按事件 */
        bool has_click_bind() const
        {
            return has_bind(button::Button_Event::kSignal_Pressed_Click) ||
                   has_bind(button::Button_Event::kDouble_Pressed_Click) ||
                   has_bind(button::Button_Event::kPressed_Repeat_Click);
        }

        /**
         * @brief 是否为结算类短按事件 (松开 500ms 后发): 单击 / 双击 / 重复点击
         * @param[in] event button::Button_Event 待查事件
         * @return bool 属于结算类返回 true
         */
        static bool is_click_event(button::Button_Event event)
        {
            return (event == button::Button_Event::kSignal_Pressed_Click) ||
                   (event == button::Button_Event::kDouble_Pressed_Click) ||
                   (event == button::Button_Event::kPressed_Repeat_Click);
        }

        const char*               label_   = nullptr; /**< 按键 DTS label */
        BridgeData                data_{};            /**< 按键线程 ↔ UI 线程的桥接数据 */
        etl::optional<LvglButton> button_{};          /**< 本颗按键 (按键线程 open 时建) */
        lv_indev_t*               indev_   = nullptr; /**< keypad 输入设备 */
        lv_group_t*               group_   = nullptr; /**< 焦点组 */
        bool                      pressed_ = false;   /**< 已吐 PRESSED, 待补 RELEASED (UI 线程独占) */
    };

    /**
     * @brief 指针后端: 鼠标 / 触屏; 无焦点组 —— 点谁谁得焦点(靠 CLICK_FOCUSABLE 标志)
     * @note  采样回调由平台给(PC 是窗口鼠标, 真机是触摸驱动), 每周期被 LVGL 调一次,
     *        必须是"读当前状态"式写法, 不许阻塞或做重活
     */
    class PointerInput final : public InputBackend
    {
    public:
        /** @brief 采样回调: 把当前指针状态写进 data (坐标 + 按下/松开) */
        using ReadCb = void (*)(lv_indev_data_t* data);

        /**
         * @brief 构造
         * @param[in] read_cb ReadCb 采样回调 (不能为空, attach 时校验)
         */
        explicit PointerInput(ReadCb read_cb) : read_cb_(read_cb) {}

        bool attach(lv_display_t* disp) override
        {
            if (disp == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "pointer: disp 为空, 拒绝 (空会退回默认屏)");
                return false;
            }
            if (this->read_cb_ == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "pointer: 采样回调为空, 拒绝");
                return false;
            }
            if (this->indev_ != nullptr)
            {
                return true; /* 幂等 */
            }

            this->indev_ = lv_indev_create();
            if (this->indev_ == nullptr)
            {
                MT_LOG_ERROR(k_tag_input, "pointer: indev create failed");
                return false;
            }
            lv_indev_set_type(this->indev_, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(this->indev_, PointerInput::read_trampoline);
            lv_indev_set_user_data(this->indev_, this);
            lv_indev_set_display(this->indev_, disp); /* 关键: 不绑就只作用在默认屏 */

            MT_LOG_INFO(k_tag_input, "pointer attached");
            return true;
        }

        void detach() override
        {
            if (this->indev_ != nullptr)
            {
                lv_indev_delete(this->indev_);
                this->indev_ = nullptr;
            }
        }

        lv_group_t* group() override { return nullptr; } /**< 指针不玩焦点 */
        bool        ready() const override { return this->indev_ != nullptr; }

        /** @brief 底层 indev 句柄 (诊断/特例用, 正常没人需要) */
        lv_indev_t* indev() const { return this->indev_; }

    private:
        /**
         * @brief LVGL read 回调适配 (user_data = this)
         * @param[in]  indev lv_indev_t*     本输入设备
         * @param[out] data  lv_indev_data_t* 写入指针状态
         */
        static void read_trampoline(lv_indev_t* indev, lv_indev_data_t* data)
        {
            auto* self = static_cast<PointerInput*>(lv_indev_get_user_data(indev));
            if ((self != nullptr) && (self->read_cb_ != nullptr))
            {
                self->read_cb_(data);
            }
        }

        ReadCb      read_cb_ = nullptr; /**< 平台采样回调 */
        lv_indev_t* indev_   = nullptr; /**< pointer 输入设备 */
    };
} // namespace ui

#endif // INPUT_BACKEND_HPP
