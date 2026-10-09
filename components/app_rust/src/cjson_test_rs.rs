/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file cjson_test_rs.rs
 * @brief cJSON 业务任务: 周期解析一段 JSON 并打印关键字段
 */
use crate::sys::json::{error_pos, JsonDoc};
use crate::sys::{delay_ms_rust, log_error, log_info, log_warn, task_manager_create_task_rust, CORE_ANY};
use core::ffi::{CStr, c_void};
use core::ptr;

const TAG: &str = "cjson_test";
const TASK_NAME: &CStr = c"cjson_test_task";
const JSON_TEXT: &CStr = c"{\"name\":\"esp32s3\",\"id\":42,\"on\":true}";

const TASK_STACK: u32 = 4096;
const TASK_PRIORITY: u32 = 5;

/* 解析成功后驻留时长 / 一轮循环间隔 */
const HOLD_MS: u32 = 1000;
const PERIOD_MS: u32 = 500;

/// 任务入口: 反复解析 JSON_TEXT
extern "C" fn task_entry_rs(_param: *mut c_void) {
    loop {
        let Some(doc) = JsonDoc::parse(JSON_TEXT) else {
            match error_pos() {
                Some(pos) => log_error!(TAG, "cJSON_Parse failed: {}", pos.to_str().unwrap_or("?")),
                None => log_error!(TAG, "cJSON_Parse failed: unknown"),
            }
            return;
        };

        let name = doc.get(c"name");
        let id = doc.get(c"id");
        let on = doc.get(c"on");

        /* name 是字符串才继续取其余字段 (与原 C++ 版判定顺序一致) */
        if let Some(name) = name.as_ref().and_then(|n| n.as_str()) {
            log_info!(TAG, "name={}", name.to_str().unwrap_or("?"));
            delay_ms_rust(HOLD_MS);

            if let Some(v) = on.as_ref().and_then(|n| n.as_f64()) {
                log_warn!(TAG, "judgment={}", v);
            }
            if let Some(v) = id.as_ref().and_then(|n| n.as_f64()) {
                log_warn!(TAG, "id={}", v);
            }
        }

        /* doc 析构自动 cJSON_Delete */
        delay_ms_rust(PERIOD_MS);
    }
}

/**
 * @brief 注册 cJSON 解析任务
 * @return 成功返回 true; 失败返回 false
 */
pub fn app_cjson_task_entry_rs() -> bool {
    task_manager_create_task_rust(TASK_NAME, TASK_STACK, TASK_PRIORITY, task_entry_rs, ptr::null_mut(), CORE_ANY).is_some()
}