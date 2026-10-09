/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file main_rust.rs
 * @brief 应用总调用入口: 只做点火 + 任务注册, 业务逻辑在各业务模块
 * @note app_main 由 IDF 的 app_startup.c 强引用, 故 Rust 侧直接导出该符号
 */
use crate::start_rs::start_rs;

/**
 * @brief 总调用入口
 * @return 同 start_rs
 */
pub fn main_rust() -> bool 
{
    start_rs()
}

/// ESP-IDF 应用入口
#[unsafe(no_mangle)]
pub extern "C" fn app_main() 
{
    main_rust();
}