/**
 * SPDX-License-Identifier: Apache-2.0
 * cpp通用按键库函数
 * 消抖我默认是设置的如果你用了RC硬件消抖可以把消抖直接设为0直接就关闭消抖了
 */
#pragma once
#include <cstdint>
#include <errno.h>
#include "osal.h"

namespace button 
{
inline uint32_t bit = 0;                                                       /*<按键 id 分配位图(1<<(id-1) 对应一个按键)*/

/**
 * @brief 计算最低位 1 的位置
 * @param x 输入值
 * @return 最低位 1 所在位索引
 */
static inline __attribute__((always_inline)) uint8_t ctz(uint32_t x)
{
    return static_cast<uint8_t>(__builtin_ctz(x));
}

constexpr uint8_t  kScan_Freq_Ms                  = 50;                       /*<扫描周期(毫秒)*/

constexpr uint16_t kMultiple_Click_Interval_Ms    = 500;                      /*<多次点击判定间隔(毫秒)*/

constexpr uint8_t  kDebounce_Ms                   = 12;                       /*<默认消抖时长(毫秒)*/

constexpr uint16_t kShort_Trigger_Ms              = 50;                       /*<默认短按触发时间(毫秒)*/

constexpr uint16_t kLong_Trigger_Ms               = 1000;                     /*<默认长按触发时间(毫秒)*/

constexpr uint16_t kLong_Hold_Trigger_Ms          = 3000;                     /*<默认长按保持时间(毫秒)*/

constexpr uint8_t  kRepeat_Click_Threshold        = 3;                        /*<连击判定阈值(>=3 次算连击)*/

constexpr uint8_t  kNot_Pressed                   = 0;                        /*<未按下电平值*/

constexpr uint8_t  kPressed                       = 1;                        /*<按下电平值*/

/**
 * @brief 按键事件类型
 */
enum class Button_Event
{
    kPressed_Down              = 1,                                            /*<首次按下*/
    kSignal_Pressed_Click      ,                                               /*<单击完成*/
    kDouble_Pressed_Click      ,                                               /*<双击完成*/
    kPressed_Repeat_Click      ,                                               /*<连击*/
    kPressed_Short_Start       ,                                               /*<短按开始(触发1次)*/
    kPressed_Short_Over        ,                                               /*<短按结束*/
    kPressed_Long_Start        ,                                               /*<长按开始*/
    kPressed_Long_Over         ,                                               /*<长按结束(触发一次)*/
    kPressed_Long_Hold         ,                                               /*<长按保持*/
    kPressed_Long_Hold_Over    ,                                               /*<长按保持结束*/
    kPressed_None              ,                                               /*<无事发生*/
    kPressed_Event_Num         ,                                               /*<按键事件数量*/
};

/**
 * @brief 按键状态机状态
 */
enum class Button_Status
{
    kPressed                   = 0,                                            /*<按下态*/
    kMultiple_Click            ,                                               /*<多点击态*/
    kNone                      ,                                               /*<未按(默认态)*/
};

constexpr uint8_t kBit_Num = 32;                                                    /*<位图总位数(最大按键数)*/

/**
 * @brief 判断某个 id 是否已占用
 * @param id 按键 id(从 1 开始)
 * @return true 已占用,false 空闲
 */
static inline bool bit_is_used(uint8_t id)
{
    return (bit >> (id - 1U)) & 1U;
}

/**
 * @brief 判断位图是否已满(所有位被占用)
 * @return true 已满,false 有空闲
 */
static inline bool bit_is_full()
{
    return ~bit == 0U;
}

/**
 * @brief 判断是否有任何按键已创建
 * @return true 至少有 1 个,false 无
 */
static inline bool bit_has_any()
{
    return bit != 0U;
}

/**
 * @brief 清空位图(重置所有 id 占用状态)
 */
static inline void bit_clear()
{
    bit = 0U;
}

/**
 * @brief 分配一个空闲 id
 * @return 分配到的 id(1~32),位图已满返回 0
 */
static inline uint8_t bit_alloc()
{
    if(bit_is_full())
        return 0U;                                  /* 已满,分配失败 */
    return static_cast<uint8_t>(ctz(~bit) + 1U);
}

/**
 * @brief 释放指定 id
 * @param id 按键 id(从 1 开始)
 */
static inline void bit_free(uint8_t id)
{
    bit &= ~(1U << (id - 1U));
}

/**
 * @brief 获取下一个已占用 id(供遍历)
 * @param from 起始 id(从 1 开始),内部按位迭代
 * @return 下一个已占用 id,无则返回 0
 */
static inline uint8_t bit_next(uint8_t from)
{
    uint32_t mask = bit >> (from - 1U);             /* 跳过 from 之前 */
    if(mask == 0U)
        return 0U;
    return static_cast<uint8_t>(ctz(mask) + from);
}

/**
 * @brief 返回系统毫秒 tick,供消抖使用
 * @return 系统启动以来的毫秒数
 */
static inline int get_tick();

/**
 * @brief 按键类:管理单个按键的状态机与事件回调
 * @tparam UserData 用户数据类型(回调传参,类型安全)
 */
template <class UserData>
class Button 
{
public:
    /**@brief 构造:分配 id 并挂入链表*/
    Button()
    {
        id = bit_alloc();                    /* 满员时返回 0,不挂链表 */
        if(id != 0U)
        {
            list_append(this);
        }
    }

    /**@brief 析构:释放 id 并从链表移除*/
    ~Button()
    {
        if(id != 0U)
            bit_free(id);
        list_remove(this);
    }

    /**@brief 回调类型:用户回调函数 + 类型安全用户数据指针(可为空)*/
    using Button_Callback = bool (*)(UserData* user, Button& self);

    uint16_t max_multiple_clicks_interval_ms = kMultiple_Click_Interval_Ms;          /*<多次点击判定间隔(毫秒)*/

    uint16_t debounce_ms                     = kDebounce_Ms;                         /*<消抖时长(毫秒)*/

    uint32_t debounce_start_tick             = 0;                                    /*<消抖计时起点(tick)*/

    uint16_t short_press_trigger_ms          = kShort_Trigger_Ms;                    /*<短按触发时间(毫秒)*/

    uint16_t long_press_trigger_ms           = kLong_Trigger_Ms;                     /*<长按触发时间(毫秒)*/

    uint16_t long_hold_trigger_ms            = kLong_Hold_Trigger_Ms;                /*<长按保持触发时间(毫秒)*/

    /**
     * @brief 外侧回调注册入口
     * @param Callback 回调函数
     * @param user 用户数据指针(类型安全,替代 void*)
     * @return 是否注册成功
     */
    bool callback_register(Button_Callback Callback, UserData* user = nullptr)
    {
        event_cb = Callback;
        user_data = user;
        return true;
    }

    /**
     * @brief 读取当前事件
     * @return 当前事件值
     */
    Button_Event event_read()
    {
        return event;
    }

    /**
     * @brief 读取按钮 id
     * @return id 值
     */
    uint8_t read_id()
    {
        return id;
    }

    /**
     * @brief 读取当前状态机状态
     * @return 当前状态
     */
    Button_Status read_status()
    {
        return status;
    }

    /**
     * @brief 读取当前是否按下
     * @return pressed_value 值(0 未按/1 按下)
     */
    uint8_t read_pressed()
    {
        return pressed_value;
    }

    /**
     * @brief 推进一次状态机
     * @return 恒为 true
     */
    bool update()
    {
        /* 非默认态:每扫描周期累加一个扫描周期对应的毫秒数,防溢出封顶到长按保持阈值 */
        if(status != Button_Status::kNone)
        {
            scan_cnt += kScan_Freq_Ms;
            if(scan_cnt >= UINT16_MAX)
                scan_cnt = long_hold_trigger_ms;
        }

        switch(status)
        {
        case Button_Status::kNone:
            /* 默认态:检测到按下 → 触发首次按下,转按下态 */
            if(is_preesed())
            {
                scan_cnt = 0;
                click_cnt = 0;
                trigger(Button_Event::kPressed_Down);
                status = Button_Status::kPressed;
            }
            else
            {
                event = Button_Event::kPressed_None;
            }
            break;

        case Button_Status::kPressed:
            if(is_preesed())
            {
                /* 仍按住:先做长按/短按判定 */
                if(scan_cnt >= long_hold_trigger_ms)
                {
                    /* 达到长按保持:仅触发一次 */
                    if(event != Button_Event::kPressed_Long_Hold)
                        trigger(Button_Event::kPressed_Long_Hold);
                }
                else if(scan_cnt >= long_press_trigger_ms)
                {
                    /* 达到长按开始:仅触发一次 */
                    if(event != Button_Event::kPressed_Long_Start)
                        trigger(Button_Event::kPressed_Long_Start);
                }
                else if(scan_cnt >= short_press_trigger_ms)
                {
                    /* 达到短按开始:仅触发一次 */
                    if(event != Button_Event::kPressed_Short_Start)
                        trigger(Button_Event::kPressed_Short_Start);
                }
            }
            else
            {
                /* 释放:按累计时间决定触发事件 */
                if(scan_cnt >= long_hold_trigger_ms)
                {
                    trigger(Button_Event::kPressed_Long_Hold_Over);
                    event = Button_Event::kPressed_None;
                    status = Button_Status::kNone;
                }
                else if(scan_cnt >= long_press_trigger_ms)
                {
                    trigger(Button_Event::kPressed_Long_Over);
                    event = Button_Event::kPressed_None;
                    status = Button_Status::kNone;
                }
                else if(scan_cnt >= short_press_trigger_ms)
                {
                    trigger(Button_Event::kPressed_Short_Over);
                    /* 短按释放:进入多次点击判定态等待连击,累加点击次数 */
                    scan_cnt = 0;
                    click_cnt++;
                    status = Button_Status::kMultiple_Click;
                }
                else
                {
                    /* 按下太短:转多次点击判定态,累加点击次数 */
                    scan_cnt = 0;
                    click_cnt++;
                    status = Button_Status::kMultiple_Click;
                }
            }
            break;

        case Button_Status::kMultiple_Click:
            /* 多次点击判定态 */
            if(is_preesed())
            {
                /* 再次按下:回按下态,重新计数 */
                scan_cnt = 0;
                status = Button_Status::kPressed;
            }
            else
            {
                /* 超过连击间隔未再按:结算单击/双击/连击事件 */
                if(scan_cnt > max_multiple_clicks_interval_ms)
                {
                    trigger(click_cnt >= kRepeat_Click_Threshold
                                ? Button_Event::kPressed_Repeat_Click :
                            click_cnt >= 2
                                ? Button_Event::kDouble_Pressed_Click :
                                  Button_Event::kSignal_Pressed_Click);
                    click_cnt = 0;
                    event = Button_Event::kPressed_None;   /* 结算后清事件 */
                    status = Button_Status::kNone;
                }
            }
            break;

        default:
            break;
        }
        return true;
    }

    /**
     * @brief 遍历链表,驱动所有按键状态机
     * @return 当前处于活动状态(非默认态)的按键数
     */
    static uint8_t scan()
    {
        uint8_t active_cnt = 0;
        Button* current_node = s_head;

        while(current_node)
        {
            current_node->update();
            if(current_node->status != Button_Status::kNone)
                active_cnt++;
            current_node = current_node->next;
        }
        return active_cnt;
    }

private:
    Button_Callback event_cb;                                                       /*<回调函数指针*/

    UserData*       user_data               = nullptr;                              /*<类型安全用户数据*/

    static Button*  s_head                  = nullptr;                              /*<单链表头*/

    Button*         next                    = nullptr;                              /*<链表节点指针*/

    uint16_t scan_cnt                       = 0;                                    /*<扫描计数(按下累计毫秒)*/

    uint16_t click_cnt                      = 0;                                    /*<点击次数(用于单击/双击/连击结算)*/

    uint8_t  id                             = 0;                                    /*<按键 id(编译时 ctz 分配)*/

    Button_Status status                    = Button_Status::kNone;                 /*<当前状态机状态*/

    Button_Event  event                     = Button_Event::kPressed_None;          /*<当前触发事件*/

    uint8_t  pressed_value                  = kNot_Pressed;                         /*<当前是否按下(0 未按/1 按下)*/

    /**
     * @brief 设置事件并触发回调
     * @param evt 要触发的事件
     */
    void trigger(Button_Event evt)
    {
        event = evt;
        if(event_cb)
            event_cb(user_data, *this);
    }

    /**
     * @brief 将按钮挂入链表(头插法)
     * @param self 按钮对象
     */
    static void list_append(Button* self)
    {
        self->next = s_head;
        s_head = self;
    }

    /**
     * @brief 从链表移除按钮
     * @param self 按钮对象
     */
    static void list_remove(Button* self)
    {
        Button** cur = &s_head;
        while (*cur)
        {
            if (*cur == self)
            {
                *cur = self->next;
                self->next = nullptr;
                return;
            }
            cur = &(*cur)->next;
        }
    }

    /**
     * @brief 消抖判定:电平需稳定 debounce_ms 才确认按下;debounce_ms==0 不消抖
     * @return 是否确认按下
     */
    bool is_preesed()
    {
        uint32_t now = static_cast<uint32_t>(get_tick());

        if(pressed_value == kPressed)
        {
            if(debounce_ms == 0)
                return true;                       /* 消抖关闭:直接返回 */
            if(debounce_start_tick == 0)
                debounce_start_tick = now;         /* 记录本次按下开始时刻 */
            return (now - debounce_start_tick) >= debounce_ms;  /* 持续 debounce_ms 才确认 */
        }
        else
        {
            debounce_start_tick = 0;               /* 松开,重置消抖计时 */
            return false;
        }
    }
}__attribute__((aligned(4)));

/**
 * @brief 读取已分配按钮数量
 * @return 按钮个数
 */
inline uint8_t read_num()
{
    return static_cast<uint8_t>(__builtin_popcount(bit));
}

/**
 * @brief 返回系统毫秒 tick(通过 OSAL 获取)
 * @return 系统启动以来的毫秒数
 */
static inline int get_tick()
{
    return static_cast<int>(osal_time_ms());
}
}
