#ifndef APP_UI_HPP
#define APP_UI_HPP
#include "lvgl_port.hpp"
namespace app 
{
    class Ui
    {
    public:
        /** @brief 只做一件事: 按 label/occupy 配好本屏 port 单例 */
        Ui(const char* panel_label, std::uint16_t panel_occupy);
        ~Ui() = default;
        Ui(const Ui&) = delete;
        Ui& operator=(const Ui&) = delete;
        Ui(Ui&&) = delete;
        Ui& operator=(Ui&&) = delete;

        /** @brief 线程入口: 拉起本屏后跑 lv_timer_handler; param 不用 */
        static void Thread(void* param);
        bool ThreadRegister(void);
    private:
        /** @brief 应用层填模板: 色号 / 第几块屏 / 缓冲容量 */
        using PanelPort = ui::LvglPort<LV_COLOR_FORMAT_RGB565_SWAPPED, 0U, 16384U>;
    };
}
#endif /* APP_UI_HPP */
