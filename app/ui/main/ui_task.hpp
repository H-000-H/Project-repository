#ifndef UI_TASK_HPP
#define UI_TASK_HPP
#include "input_backend.hpp"
#include "lock_screen.hpp"
#include "lvgl_port.hpp"
#include "setting_main.hpp"
#include "ui_app.hpp"
#include <cstdint>

namespace app
{
    /**
     * @brief UI 任务: 组合根(显示 port + 外壳 + 页面) + 线程壳
     * @note  成员声明顺序 = 构造顺序, 别随便挪: 页面构造时要拿 App&, 所以 app_ 必须排在
     *        lock_ / setting_ 前面。
     * @note  驱动收敛在一个线程: lv_timer_handler 里有 already_running 自旋保护, 并发调用
     *        会被直接顶掉 —— 所以别打算"多开几个 UI 线程"。
     */
    class UiTask
    {
    public:
        /**
         * @brief 本屏的显示 port
         * @note  id 是喂给 LvglPort 的屏号: 刷屏回调靠 `self()` 这个**模板静态**指针找单例,
         *        真要开第二块物理屏必须换一个 id 再实例化(DTS 也要加节点), 否则两块屏共用一份 port
         */
        using PanelPort = ui::LvglPort<LV_COLOR_FORMAT_RGB565_SWAPPED, 0U, ui::kDefaultFlushBufferBytes>;

        static constexpr std::uint32_t k_stack_size     = 8192U; /**< UI 线程栈 (字节) */
        static constexpr std::uint8_t  k_priority       = 12U;   /**< UI 线程优先级 */
        static constexpr std::uint32_t k_loop_period_ms = 10U;   /**< 主循环节拍, 与 lv_timer_handler 同拍 */

        /**
         * @brief 构造: 绑本屏的显示 port (只配单例, 不碰 LVGL 运行时)
         * @param[in] panel_label  const char*   面板设备 label
         * @param[in] panel_occupy std::uint16_t 第几块屏 (占用号)
         */
        UiTask(const char* panel_label, std::uint16_t panel_occupy);
        ~UiTask() = default; /**< 默认析构 */

        UiTask(const UiTask&)            = delete; /**< 禁用拷贝构造 */
        UiTask& operator=(const UiTask&) = delete; /**< 禁用拷贝赋值 */
        UiTask(UiTask&&)                 = delete; /**< 禁用移动构造 */
        UiTask& operator=(UiTask&&)      = delete; /**< 禁用移动赋值 */

        /**
         * @brief 线程体: 起本屏 → 死循环(驱动页面 + 唯一一次 lv_timer_handler)
         * @param[in] param void* UiTask 实例 (thread_register 传进来的 this)
         */
        static void thread(void* param);

        /**
         * @brief 注册 UI 任务到调度器
         * @param[in] input ui::InputBackend& 输入后端 (真机给 KeypadInput, PC 给 PointerInput)
         * @return bool 创建成功返回 true, 失败返回 false
         * @note  后端生命周期必须长于本任务: 本类只存指针, 不接管所有权
         */
        bool thread_register(ui::InputBackend& input);

    private:
        /**
         * @brief 起本屏: port.init → app.init → 顶栏 → 页表 → 首页
         * @return bool 就绪返回 true
         * @note  只能在 UI 线程调 (里面会建 LVGL 对象)
         */
        bool prepare();

        PanelPort&        port_;            /**< 显示 port 单例 (构造时绑, 非拥有) */
        ui::App           app_{};           /**< 外壳 + 页表 + 返回栈 + 输入后端 */
        ui::LockScreen    lock_{app_};      /**< 页面: 锁屏 */
        ui::SettingMain   setting_{app_};   /**< 页面: 设置 */
        ui::InputBackend* input_ = nullptr; /**< 输入后端 (非拥有指针) */
    };
}
#endif /* UI_TASK_HPP */
