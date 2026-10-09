#![no_std]
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file lib.rs
 * @brief crate 根: 模块声明 + panic handler
 */
mod cjson_test_rs;
mod main_rust;
mod start_rs;
mod sys;
mod ws2812_rs;

use core::panic::PanicInfo;

/* no_std 静态库必须提供 panic handler; 嵌入式禁异常, 直接停机 */
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! 
{
    loop {}
}