/**
 * @file ws2812_rs.rs
 * @brief WS2812 业务任务: 6 色循环, 亮/灭各半周期
 * @author H-000-H
 * @Copyright SPDX-License-Identifier: Apache-2.0
 */
use crate::sys::{device_close_rust, device_from_ptr_rust, device_get_name_rust, device_ioctl_none_rust, device_ioctl_rust, device_open_rust, delay_ms_rust, log_error, log_info, log_warn, task_manager_create_task_rust, CORE_ANY};
use core::ffi::{CStr, c_int, c_void};

const TAG: &str = "AppWs2812";
const DEV_LABEL: &CStr = c"ws2812_rgb";
const TASK_NAME: &CStr = c"ws2812";

const HALF_PERIOD_MS: u32 = 500;
const IOCTL_TIMEOUT_MS: u32 = 100;
const TASK_STACK: u32 = 2048;
const TASK_PRIORITY: u32 = 5;

/**
 * ioctl 命令号
 * @note 原为 COMPAT_MAGIC(WS2812), 该宏随 osal 层一并删除。按 compiler_compat.h
 *       的 MINI_MAGIC_TABLE 换算: WS2812 槽号 0x10 x 步长 0x100 = 0x1000。
 */
const CMD_SET_RGB: c_int = 0x1001;
const CMD_CLEAR: c_int = 0x1002;

/// 灯珠参数包, 布局须与 C 的 struct ws2812_rgb_arg 逐字节一致
#[repr(C)]
struct RgbArg {
    index: u32,
    r: u8,
    g: u8,
    b: u8,
}

/// 调色板: 红 / 绿 / 蓝 / 黄 / 青 / 品红, 亮度取 16 避免过曝
const COLORS: [[u8; 3]; 6] = [
    [16, 0, 0],
    [0, 16, 0],
    [0, 0, 16],
    [16, 16, 0],
    [0, 16, 16],
    [16, 0, 16],
];

/// 任务入口: 6 色循环
extern "C" fn task_entry_rs(param: *mut c_void) 
{
    let dev = unsafe { device_from_ptr_rust(param) };
    let mut idx: usize = 0;

    loop {
        let (r, g, b) = (COLORS[idx][0], COLORS[idx][1], COLORS[idx][2]);
        let mut arg = RgbArg { index: 0, r, g, b };
        if device_ioctl_rust(&dev, CMD_SET_RGB, &mut arg, IOCTL_TIMEOUT_MS).is_err() {
            log_error!(TAG, "SET_RGB failed idx={}", idx);
        }
        delay_ms_rust(HALF_PERIOD_MS);

        if device_ioctl_none_rust(&dev, CMD_CLEAR, IOCTL_TIMEOUT_MS).is_err() {
            log_error!(TAG, "CLEAR failed");
        }
        delay_ms_rust(HALF_PERIOD_MS);

        idx = (idx + 1) % COLORS.len();
    }
}

/**
 * @brief 打开 ws2812 设备并注册 6 色闪烁任务
 * @return 成功返回 true; 失败返回 false
 */
pub fn app_ws2812_task_start_rs() -> bool {
    let dev = match device_open_rust(DEV_LABEL) {
        Ok(d) => d,
        Err(_) => {
            log_warn!(TAG, "device 'ws2812_rgb' not found");
            return false;
        }
    };

    match task_manager_create_task_rust(TASK_NAME, TASK_STACK, TASK_PRIORITY, task_entry_rs, dev.as_ptr(), CORE_ANY) {
        Some(handle) => {
            log_info!(TAG, "started, task={:p}, device={}", handle.as_ptr(), device_get_name_rust(&dev).to_str().unwrap_or("?"));
            true
        }
        None => {
            log_error!(TAG, "task create failed");
            if let Err(err) = device_close_rust(&dev) {
                log_error!(TAG, "device_close failed: {}", err);
            }
            false
        }
    }
}
