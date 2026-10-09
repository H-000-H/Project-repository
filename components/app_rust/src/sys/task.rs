/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file task.rs
 * @brief 任务创建与延时的 safe 封装层
 * @note 命名规则: 对应 C 符号的 safe 包装一律 <c_symbol>_rust, 与 ffi.rs 的原名对应。
 */
use crate::sys::ffi::{mini_delay_ms, task_manager_create_task};
use core::ffi::{CStr, c_int, c_void};

/// 任务句柄
pub struct TaskHandle(*mut c_void);

impl TaskHandle {
    /// @brief 原始句柄地址
    /// @return 任务句柄
    pub fn as_ptr(&self) -> *mut c_void {
        self.0
    }
}

/// 不绑定核心
pub const CORE_ANY: c_int = -1;

/**
 * @brief 创建任务 (包装 task_manager_create_task)
 * @param[in] name 任务名
 * @param[in] stack_size 栈大小 (字节)
 * @param[in] priority 优先级 (FreeRTOS 语义: 数值越大越高)
 * @param[in] entry 任务入口
 * @param[in] param 任务参数
 * @param[in] core_id 核心号, 通常传 CORE_ANY
 * @return 成功返回句柄; 创建失败返回 None
 * @note 句柄仅判空: C 侧返回的是指针, 不能与状态码 0 比较
 */
pub fn task_manager_create_task_rust(
    name: &CStr,
    stack_size: u32,
    priority: u32,
    entry: extern "C" fn(*mut c_void),
    param: *mut c_void,
    core_id: c_int,
) -> Option<TaskHandle> {
    let raw = unsafe { task_manager_create_task(name.as_ptr(), stack_size, priority, entry, param, core_id) };
    (!raw.is_null()).then_some(TaskHandle(raw))
}

/**
 * @brief 延时毫秒 (包装 mini_delay_ms, 让出调度器)
 * @param[in] ms 毫秒数
 */
pub fn delay_ms_rust(ms: u32) {
    unsafe { mini_delay_ms(ms) };
}