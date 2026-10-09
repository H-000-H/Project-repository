/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file mod.rs
 * @brief 系统层: FFI + safe 封装 (设备 / 任务 / 日志 / JSON)
 */
pub mod device;
pub mod ffi;
pub mod json;
pub mod log;
pub mod task;

pub use device::{
    device_close_rust, device_from_ptr_rust, device_get_name_rust, device_ioctl_none_rust, device_ioctl_rust,
    device_open_rust,
};
pub use task::{CORE_ANY, delay_ms_rust, task_manager_create_task_rust};

/* 日志宏在 log 模块内是 pub(crate) 重导出, 此处只能同样按 crate 可见性再导出 */
pub(crate) use log::{log_error, log_info, log_warn};