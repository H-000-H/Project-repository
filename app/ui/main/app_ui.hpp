#ifndef APP_UI_HPP
#define APP_UI_HPP
#include "lvgl_port.hpp"
namespace app 
{
    class Ui
    {
    public:
        /**
         * @brief 只做一件事: 按 label/occupy 配好本屏 port 单例
         * @param panel_label  面板设备 label
         * @param panel_occupy 第几块屏 (占用号)
         */
        Ui(const char* panel_label, std::uint16_t panel_occupy);
        ~Ui() = default;                 /**< 默认析构 */
        Ui(const Ui&) = delete;          /**< 禁用拷贝构造 */
        Ui& operator=(const Ui&) = delete; /**< 禁用拷贝赋值 */
        Ui(Ui&&) = delete;               /**< 禁用移动构造 */
        Ui& operator=(Ui&&) = delete;    /**< 禁用移动赋值 */

        /**
         * @brief UI 任务体: 初始化 port/输入设备/页面后死循环驱动 lv_timer_handler
         * @param param 线程参数 (未使用)
         */
        static void Thread(void* param);
        /**
         * @brief  注册 UI 任务到调度器 (mini-os 线程)
         * @return 创建成功返回 true, 失败返回 false
         */
        bool ThreadRegister(void);
    private:
        /** @brief 应用层填模板: 色号 / 第几块屏 / 缓冲容量 */
        using PanelPort = ui::LvglPort<LV_COLOR_FORMAT_RGB565_SWAPPED, 0U, 16384U>;
    };
}
#endif /* APP_UI_HPP */
