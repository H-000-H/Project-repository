/*界面按照file系统分home就是锁屏页面->然后按照目录一样划分不同深度的页面半遵循MVP不会全按MVP写*/
#include "app_ui.hpp"
#include "mini_time.h"
#include "schedule.h"
#include "system_log.h"
#include "thread.h"
#include <cstdint>
#include "../indev/lvgl_button_bridge.hpp"
#include <lvgl/core/lv_group.h>
#include <lvgl/core/lv_obj_pos.h>
#include <lvgl/display/lv_display.h>
#include <lvgl/indev/lv_indev.h>
#include <lvgl/widgets/lv_button.h>
#include "../global/dektop/dektop.hpp"
#include "../global/top-bar/top-bar.hpp"
#include "../home/home.hpp"
#include "../home/lock/lock.hpp"
namespace app
{
    constexpr const char *      kUiThreadName       = "UiThread";
    constexpr const uint32_t    kUiThreadStackSize  = 8192;
    constexpr const uint8_t     kUiThreadPriority   = 12;
    /**
     * @brief 构造: 按 label/occupy 配好本屏 port 单例
     * @param panel_label  面板设备 label
     * @param panel_occupy 第几块屏 (占用号)
     */
    Ui::Ui(const char* panel_label, std::uint16_t panel_occupy)
    {
        (void)PanelPort::get_instance(panel_label, panel_occupy);
    }

    /**
     * @brief UI 任务体: 初始化 port/输入设备/页面后死循环驱动 lv_timer_handler
     * @param param 线程参数 (未使用)
     */
    void Ui::Thread(void* param)
    {
        (void)param;

        auto& port = PanelPort::instance();
        const int init_ret = port.Init();
        if (init_ret != MINI_OK)
        {
            MT_LOG_ERROR(kUiThreadName, "port init failed: %d, ui thread exit", init_ret);
            return;
        }
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(indev, ui::ButtonLvglIndevRead);

        /*创建焦点组*/
        lv_group_t* group = lv_group_create();
        /*开焦点循环*/
        lv_group_set_wrap(group,true);
        lv_indev_set_group(indev,group);

        /*测试*/
        lv_obj_t *btn =lv_button_create(lv_screen_active());
        lv_obj_set_size(btn,20 ,60);
        lv_obj_center(btn);
        lv_group_add_obj(group,btn);
        lv_group_focus_obj(btn);
        for(uint8_t t = 0; (t<20)&& !ui::ButtonLvglReady(ui::kButtonLvglButton2);++t)
        {
             mini_os_schedule_delay(MINI_OS_MS_TO_TICK(10));
        }
        ui::ButtonLvglClear(ui::kButtonLvglButton2);
        ui::ButtonLvglBind(ui::kButtonLvglButton2,button::Button_Event::kPressed_Short_Over,LV_KEY_ENTER);

        auto& desktop = ui::Desktop::GetInstance();
        desktop.CreateDesktop();
        desktop.CreateTopText("Unlock with any operation");

        static ui::Lock* s_lock = new ui::Lock;
        s_lock->CreateLockScreen(desktop.GetScreen());

        /* 顶栏: 左段两项(宽度自适应) + 右段一项(固定 44px)*/
        static ui::TopBar s_topbar;
        s_topbar.createbarscreen();
        s_topbar.addobj(LV_SYMBOL_WIFI);   /* width 0 = 按文字宽度; is_head=true = 左段 */
        s_topbar.addobj("20:31", 0, true);
        s_topbar.addobj("88%");         /* 固定 44px; is_head 用默认(false) = 右段 */
        auto& password = app::LockPassword::GetInstance();
        password.Set("1234567");

        /* 延时驱动 UI 只能在循环里按时间戳判断: 阻塞延时会把 lv_timer_handler() 停掉, 一帧都不画(黑屏) */
        const uint32_t t0 = mini_time_ms();
        bool tried_wrong = false;
        bool tried_right = false;
        bool unlocked = false;
        for(;;)
        {
            const uint32_t now = mini_time_ms();
            if((s_lock != nullptr) && !tried_wrong && ((now - t0) >= 4000U))
            {
                s_lock->Unlock("123456");      /* 错密码 → 弹红提示 */
                tried_wrong = true;
            }
            if((s_lock != nullptr) && !tried_right && ((now - t0) >= 8000U))
            {
                unlocked = s_lock->Unlock("1234567");   /* 对密码 → 弹绿色 Welcome(1.5s 后自己收走) */
                tried_right = true;
            }

            if((s_lock != nullptr) && unlocked && !s_lock->IsTipShowing())
            {
                s_lock->DeleteLockScreen();
                s_lock = nullptr;
            }

            if (port.disp != nullptr)
            {
                lv_timer_handler();
            }
            
            mini_os_schedule_delay(MINI_OS_MS_TO_TICK(10));
        }
    }

    /**
     * @brief  注册 UI 任务到调度器 (mini-os 线程)
     * @return 创建成功返回 true, 失败返回 false
     */
    bool Ui::ThreadRegister(void)
    {
        auto handle = mini_os_thread_create(kUiThreadName,kUiThreadStackSize,kUiThreadPriority,Thread,nullptr);
        if(!handle)
        {
            MT_LOG_ERROR(kUiThreadName,"Ui thread register failed");
            return false; 
        }
        MT_LOG_INFO(kUiThreadName,"Ui thread register success");
        return true;
    }

} // namespace app
