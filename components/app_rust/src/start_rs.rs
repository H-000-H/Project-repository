/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file start_rs.rs
 * @brief 应用启动时序: mini_tree 初始化 + 注册业务任务
 */
use crate::cjson_test_rs::app_cjson_task_entry_rs;
use crate::sys::ffi::{board_register_all_drivers, mini_tree_pre_os_init, mini_tree_start_tasks, system_init_complete};
use crate::sys::{log_info, log_warn};
use crate::ws2812_rs::app_ws2812_task_start_rs;

const TAG: &str = "AppStart";

/**
 * @brief 应用启动: mini_tree 初始化 + 注册业务任务
 * @return 全部业务任务就绪返回 true; 任一失败返回 false
 * @note 失败路径仍调用 system_init_complete: 否则全局中断不释放, 后续
 *       TWDT / INT WDT 无法接管, 系统进不了可观测状态。
 */
pub fn start_rs() -> bool 
{
    unsafe {
        mini_tree_pre_os_init();
        board_register_all_drivers();
        mini_tree_start_tasks();
    }

    if !app_ws2812_task_start_rs() {
        log_warn!(TAG, "ws2812 task not started");
        unsafe { system_init_complete() };
        return false;
    }

    if !app_cjson_task_entry_rs() {
        log_warn!(TAG, "cjson task not started");
        unsafe { system_init_complete() };
        return false;
    }

    unsafe { system_init_complete() };
    log_info!(TAG, "app start complete");
    true
}