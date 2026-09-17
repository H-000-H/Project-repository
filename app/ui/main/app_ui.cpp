/*界面按照file系统分home就是锁屏页面->然后按照目录一样划分不同深度的页面半遵循MVP不会全按MVP写*/
#include "app_ui.hpp"
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
namespace app
{
    constexpr const char *      kUiThreadName       = "UiThread";
    constexpr const uint32_t    kUiThreadStackSize  = 8192;
    constexpr const uint8_t     kUiThreadPriority   = 12;
    Ui::Ui(const char* panel_label, std::uint16_t panel_occupy)
    {
        (void)PanelPort::get_instance(panel_label, panel_occupy);
    }

    void Ui::Thread(void* param)
    {
        (void)param;

        auto& port = PanelPort::instance();
        if (port.Init() != MINI_OK)
        {
            MT_LOG_ERROR(kUiThreadName, "port init failed");
        }
        lv_indev_t* indev = lv_indev_create();
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
        for(;;)
        {
            if (port.disp != nullptr)
            {
                lv_timer_handler();
            }
            mini_os_schedule_delay(MINI_OS_MS_TO_TICK(10));
        }
    }

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
