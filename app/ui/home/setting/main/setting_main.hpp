#ifndef SETTING_MAIN_HPP
#define SETTING_MAIN_HPP
#include "lvgl.h"
#include "page.hpp"
namespace ui
{
    class App;
    /** @brief 设置页 (默认收场方式 HIDE, 注册名 "setting") */
    class SettingMain : public Page
    {
    public:
        /**
         * @brief 构造
         * @param[in] app App& 组合根引用
         */
        explicit SettingMain(App& app);
        ~SettingMain() override;
        SettingMain(const SettingMain&)            = delete;
        SettingMain& operator=(const SettingMain&) = delete;
        SettingMain(SettingMain&&)                 = delete;
        SettingMain& operator=(SettingMain&&)      = delete;

        void enter(lv_obj_t* parent) override;
        void exit() override;
        bool is_finished() const override { return false; }

    private:
        /**
         * @brief 建设置页控件树
         * @param[in] parent lv_obj_t* 挂载父对象
         * @return bool 建成功返回 true
         */
        bool create_widgets(lv_obj_t* parent);

        /** @brief 拆控件树 (仅 exit 调用; 建树中途失败的回滚走基类 destroy_root) */
        void destroy_widgets();

        /** @brief 把焦点入口加进焦点组并聚焦 */
        void grab_focus();

        bool scroll_down(); /**< 向下滚动一屏 */

        bool scroll_up();   /**< 向上滚动一屏 */

        bool create_scroll(lv_obj_t* parent);/**<创建滚轮 */
        App&      app;
        lv_obj_t* scroll = nullptr; /**< 滚动轴 (父节点是 root) */
    };
}
#endif
