/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @author H-000-H
 * @file lvgl_button_bridge.hpp
 * @brief LVGL 按钮输入设备桥接
 * @details 该文件提供了 LVGL 按钮输入设备与底层输入事件的适配接口
 * @note lvgl 桥接口处理全部按键事件不是接入电平到lvgl, 
        而是通过事件驱动的方式, 并将其状态同步到 LVGL 输入设备中
        接入电平信号会和lvgl内部状态打架, 因此不直接接入 lvgl 输入设备中, 
        而是通过事件驱动的方式间接接入。
 */

#pragma once

#include <cstdint>
#include <etl/optional.h>
#include "compiler_compat.h" 
#include "buffer.h"         
#include "device.h"
#include "status.h"
#include "system_log.h"
#include "lvgl.h"
#include "app_button_base.hpp"
#include "lib/button.hpp" 
namespace ui
{
    /* ================= LVGL 按键: 按键 → LVGL 键值 ================= */
    /** @brief 单个事件最多绑几个键 = 连发上限 (一次触发按顺序全发完, 2 条够用就别开大) */
    constexpr uint8_t kButtonLvglEventKeyMax = 2U;
    /** @brief 事件槽数量: 我的按键事件枚举减去 2 (枚举从 1 起, 表从 0 起; 末尾 kPressed_None 是哨兵, 不进表) */
    constexpr uint8_t kButtonLvglEventNum = static_cast<uint8_t>(button::Button_Event::kPressed_Event_Num) - 2U;

    constexpr uint16_t kButtonLvglQueueLen = 8U;
    static_assert((kButtonLvglQueueLen & (kButtonLvglQueueLen - 1U)) == 0U, "队列长度必须是 2 的幂");

    struct ButtonLvglData
    {
        /* 事件 → 键值序列: 一个事件可绑多个键, 触发后按绑定顺序逐个入队 */
        lv_key_t lv_event[kButtonLvglEventNum][kButtonLvglEventKeyMax] = {};
        /* 条数跨线程发布用 (换页时 UI 线程写, 按键线程读): 先写数据, 再 release 发布条数 记录每个事件绑定多少条东西的 */
        MINI_ATOMIC_UINT8 lv_event_num[kButtonLvglEventNum] = {};

        /* 我没有用自定义长度版本而是直接用固定长度因为 fifo_spsc 内部实现是uintptr_t, 这样可以保证在 32/64 位平台上都能正确存储 lv_key_t */
        struct fifo_spsc lv_queue                         = {};
        fifo_data_type   lv_queue_buf[kButtonLvglQueueLen] = {};
        uint16_t         lv_q_drop                         = 0U;    /* 队列满被丢的键数 */
        bool             lv_pressed                        = false; /* 已吐 PRESSED, 待补 RELEASED  */
    };

    class AppButtonLvgl : public app::AppButton<ButtonLvglData>
    {
    private:
        bool ButtonCallback(button::Button& self, ButtonLvglData& data) override final
        {
            /* 枚举从 1 起, 表从 0 起; kPressed_None 已被 -2 排除在表外, 落到下面返回 false */
            const uint8_t idx = static_cast<uint8_t>(self.event_read()) - 1U;
            if (idx >= kButtonLvglEventNum)
            {
                return false;
            }

            /* 本事件绑了几个键就入队几个 (按键线程是队列唯一生产者);
               acquire 读条数 → 保证读到 num 时前 num 条数据已写完 (换页时另一个线程正在写) */
            const uint8_t num = MINI_ATOMIC_LOAD(&data.lv_event_num[idx], MINI_ACQUIRE);
            for (uint8_t i = 0; i < num; ++i)
            {
                const fifo_data_type key = static_cast<fifo_data_type>(data.lv_event[idx][i]);
                if (fifo_write_data(&data.lv_queue, key) != BUFF_OK)
                {
                    ++data.lv_q_drop; /* 满: 丢最新那个, 已入队的按顺序保留 */
                    break;
                }
            }
            return true;
        }

    public:
        /** @brief 追加一条绑定: 某个按键事件 → 一个 LVGL 键值 (0 = 不绑)
         *  @note 同一事件调多次 = 触发后按调用顺序逐个上报 (连发)
         *  @note 短按有两种表示: 松手即发的 kPressed_Short_Over, 与 500ms 后结算的 Click 系列;
         *        同一次短按会先后都发, 所以这两类之间只允许占一边, 混绑直接拒
         *  @return false = 事件非法 / 该事件已绑满 kButtonLvglEventKeyMax 条 / 短按表示冲突 */
        bool set_event_lvgl(button::Button_Event event, lv_key_t key)
        {
            const uint8_t idx = static_cast<uint8_t>(event) - 1U;
            if ((key == 0U) || (idx >= kButtonLvglEventNum) ||
                (MINI_ATOMIC_LOAD(&m_data.lv_event_num[idx], MINI_RELAXED) >= kButtonLvglEventKeyMax))
            {
                return false;
            }

            const bool bind_short_over = (event == button::Button_Event::kPressed_Short_Over);
            const bool bind_click      = is_click_event(event);
            if ((bind_short_over && has_click_event_bind()) ||
                (bind_click && has_bind(button::Button_Event::kPressed_Short_Over)))
            {
                return false; /* 短按的两种表示只能选一边, 否则一次短按会出两个动作 */
            }

            /* 写者只有绑定方 (换页那条线程), 所以先写数据、再 release 发布条数即可 */
            const uint8_t num    = MINI_ATOMIC_LOAD(&m_data.lv_event_num[idx], MINI_RELAXED);
            m_data.lv_event[idx][num] = key;
            MINI_ATOMIC_STORE(&m_data.lv_event_num[idx], static_cast<uint8_t>(num + 1U), MINI_RELEASE);
            return true;
        }

        /** @brief 清空全部绑定 (换页/换模式: 先清再重新绑)
         *  @note 只把条数置 0、不动数据表, 配合"先写数据再发布条数"的顺序, 读者任何时刻看到的
         *        都是已完整写入的前缀 —— 最坏是少触发一次, 绝不会发出半条数据 */
        void clear_bind()
        {
            for (uint8_t i = 0; i < kButtonLvglEventNum; ++i)
            {
                MINI_ATOMIC_STORE(&m_data.lv_event_num[i], 0U, MINI_RELEASE);
            }
        }

        /** @brief 给 LVGL: 一个键 = 一次 PRESSED + 一次 RELEASED
         *  @note 中间必须夹一次松开, 否则第二个键进不了"按下"分支(LVGL 只在 RELEASED→PRESSED 沿发 PRESSED)
         *  @return false = 没有待上报的键 (data 未写入) */
        bool lvgl_step(lv_indev_data_t& data)
        {
            if (m_data.lv_pressed) /* 上一步吐了按下, 这一步必须补松开 */
            {
                m_data.lv_pressed = false;
                data.state        = LV_INDEV_STATE_RELEASED; /* 键值沿用 LVGL 预填的 last_key */
                /* 队列里还有键 → 让 LVGL 在同一个周期里接着读下一个 */
                bool empty = true;
                MINI_IGNORE_RESULT(fifo_isempty(&m_data.lv_queue, &empty));
                data.continue_reading = !empty;
                return true;
            }

            fifo_data_type key = 0;
            if (fifo_read_data(&m_data.lv_queue, &key) != BUFF_OK)
            {
                return false; /* 队列空 */
            }

            data.key = static_cast<lv_key_t>(key);

            m_data.lv_pressed = true;
            data.state        = LV_INDEV_STATE_PRESSED;
            /* 本次按下要补一次松开 */
            data.continue_reading = true;
            return true;
        }

        ~AppButtonLvgl() = default;
        explicit AppButtonLvgl(const char* label, ButtonLvglData& data)
            : app::AppButton<ButtonLvglData>(label, app::kTriggerLevelLow, data)
        {
        }

    private:
        /** @brief 该事件当前是否已绑 (至少一个键) */
        bool has_bind(button::Button_Event event) const
        {
            const uint8_t i = static_cast<uint8_t>(event) - 1U;
            return (i < kButtonLvglEventNum) && (MINI_ATOMIC_LOAD(&m_data.lv_event_num[i], MINI_RELAXED) != 0U);
        }

        /** @brief 结算类短按事件 (松开 500ms 后发): 单击 / 双击 / 重复点击 */
        static bool is_click_event(button::Button_Event event)
        {
            return (event == button::Button_Event::kSignal_Pressed_Click) ||
                   (event == button::Button_Event::kDouble_Pressed_Click) ||
                   (event == button::Button_Event::kPressed_Repeat_Click);
        }

        /** @brief 是否已绑了任一结算类短按事件 */
        bool has_click_event_bind() const
        {
            return has_bind(button::Button_Event::kSignal_Pressed_Click) ||
                   has_bind(button::Button_Event::kDouble_Pressed_Click) ||
                   has_bind(button::Button_Event::kPressed_Repeat_Click);
        }
    };

    /* 要接入 LVGL 的按键: 加一颗 = 加一行 (顺序即下标, 与 ButtonLvglIndex 对应) */
    constexpr uint8_t kButtonLvglNum = 1U;

    inline constexpr const char* kButtonLvglLabels[kButtonLvglNum] = { "button2" };

    /** @brief 按键下标: 动态绑定时的入参, 免得写魔法数字 */
    enum ButtonLvglIndex : uint8_t
    {
        kButtonLvglButton2 = 0U, /* "button2" */
    };

    inline ButtonLvglData               s_lvgl_data[kButtonLvglNum] = {};
    inline etl::optional<AppButtonLvgl> s_lvgl_btn[kButtonLvglNum];

    /** @brief 清掉一颗按键的全部绑定 (换页/换模式: 先清, 再按新页面重新绑) */
    inline void ButtonLvglClear(uint8_t index)
    {
        if ((index < kButtonLvglNum) && s_lvgl_btn[index])
        {
            s_lvgl_btn[index]->clear_bind();
        }
    }

    /** @brief 绑一条: 某个按键事件 → 一个 LVGL 键值 (同一事件可连续调 = 连发)
     *  @return false = 下标非法 / 按键没注册 / 事件非法 / 该事件已绑满 / 短按表示冲突
     *  @note 运行期随时可调 (换页处调即可, 不用重启) */
    inline bool ButtonLvglBind(uint8_t index, button::Button_Event event, lv_key_t key)
    {
        if (!((index < kButtonLvglNum) && s_lvgl_btn[index]))
        {
            return false;
        }
        return s_lvgl_btn[index]->set_event_lvgl(event, key);
    }

    inline void ButtonLvglInit()
    {
        for (uint8_t i = 0; i < kButtonLvglNum; ++i)
        {
            const int ret = fifo_init(&s_lvgl_data[i].lv_queue, s_lvgl_data[i].lv_queue_buf, kButtonLvglQueueLen);
            if (ret != BUFF_OK)
            {
                MT_LOG_ERROR("lvgl_button", "fifo_init failed: %d", ret);
                continue; /* 环没建起来就不能让按键往里写, 这颗按键直接不注册 */
            }

            /* 只建对象, 不绑任何东西: 绑什么由使用方按当前页面在运行期决定 */
            s_lvgl_btn[i].emplace(kButtonLvglLabels[i], s_lvgl_data[i]);
            s_lvgl_btn[i]->RegisterCallback();
        }
    }

    /** @brief 喂一遍电平: 按键扫描线程每周期调一次 */
    inline void ButtonLvglSample()
    {
        for (uint8_t i = 0; i < kButtonLvglNum; ++i)
        {
            if (s_lvgl_btn[i])
            {
                s_lvgl_btn[i]->SampleLevel();
            }
        }
    }

    /** @brief LVGL read 回调: 有待上报的键就吐一个出去 (多键靠 continue_reading 在同一周期连着吐) */
    inline void ButtonLvglRead(lv_indev_data_t& data)
    {
        for (uint8_t i = 0; i < kButtonLvglNum; ++i)
        {
            if (s_lvgl_btn[i] && s_lvgl_btn[i]->lvgl_step(data))
            {
                return;
            }
        }
        data.state            = LV_INDEV_STATE_RELEASED;
        data.continue_reading = false;
    }

    /** @brief LVGL read 回调入口 */
    inline void ButtonLvglIndevRead(lv_indev_t* indev, lv_indev_data_t* data)
    {
        (void)indev;
        ButtonLvglRead(*data);
    }

    /** @brief 该按键是否已注册好 (可以接受动态绑定) */
    inline bool ButtonLvglReady(uint8_t index)
    {
        return (index < kButtonLvglNum) && static_cast<bool>(s_lvgl_btn[index]);
    }

} // namespace ui