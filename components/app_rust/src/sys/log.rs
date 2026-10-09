/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file log.rs
 * @brief 日志封装: 宏 (调用点) + 栈上格式化 (实现) + esp_log FFI
 */
use crate::sys::ffi::esp_log_write;
use core::ffi::{CStr, c_char, c_int};
use core::fmt::{self, Write};

/// esp_log_level_t
pub const LOG_ERROR: c_int = 1;
pub const LOG_WARN: c_int = 2;
pub const LOG_INFO: c_int = 3;

/// 单行上限 (超出丢弃), 与 mini-log 的 MINI_LOG_MAX_LEN 同量级
const LINE_MAX: usize = 192;
/// tag 上限 (esp_log 会把 tag 记入 tag level 表, 不宜过长)
const TAG_MAX: usize = 24;

/* ── 栈上格式化缓冲 ──────────────────────────────────────────────────────── */
struct Line {
    buf: [u8; LINE_MAX],
    len: usize,
}

impl Line {
    const fn new() -> Self {
        Self { buf: [0u8; LINE_MAX], len: 0 }
    }

    /// 补 '\0' 并构造 CStr
    /// @return 以 '\0' 结尾的字符串
    fn finish(&mut self) -> &CStr {
        self.buf[self.len] = 0;
        unsafe { CStr::from_ptr(self.buf.as_ptr() as *const c_char) }
    }
}

impl Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let end = self.len + s.len();
        if end >= self.buf.len() {
            return Err(fmt::Error);
        }
        self.buf[self.len..end].copy_from_slice(s.as_bytes());
        self.len = end;
        Ok(())
    }
}

/// 把 tag 写入定长缓冲并补 '\0'
/// @param[in] tag 日志标签
/// @return 以 '\0' 结尾的 tag
fn tag_cstr(tag: &str) -> Line {
    let mut t = Line::new();
    let n = tag.len().min(TAG_MAX - 1);
    t.buf[..n].copy_from_slice(&tag.as_bytes()[..n]);
    t.len = n;
    t
}

/**
 * @brief 输出一条格式化日志 (供宏调用, 不直接暴露给业务)
 * @param[in] level 日志级别
 * @param[in] tag 日志标签
 * @param[in] args 格式化参数 (在函数内同步消费)
 */
pub fn esp_log_write_rust(level: c_int, tag: &str, args: fmt::Arguments) {
    let mut t = tag_cstr(tag);
    let mut line = Line::new();
    /* 格式化失败即超长: 保留已写入部分, 不视为致命 */
    let _ = line.write_fmt(args);
    unsafe { esp_log_write(level, t.finish().as_ptr(), c"%s".as_ptr(), line.finish().as_ptr()) };
}

macro_rules! log_info {
    ($tag:expr, $($arg:tt)*) => { $crate::sys::log::esp_log_write_rust($crate::sys::log::LOG_INFO, $tag, format_args!($($arg)*)) };
}
macro_rules! log_warn {
    ($tag:expr, $($arg:tt)*) => { $crate::sys::log::esp_log_write_rust($crate::sys::log::LOG_WARN, $tag, format_args!($($arg)*)) };
}
macro_rules! log_error {
    ($tag:expr, $($arg:tt)*) => { $crate::sys::log::esp_log_write_rust($crate::sys::log::LOG_ERROR, $tag, format_args!($($arg)*)) };
}

/* 单 crate 项目: pub(crate) use 重导出即可, 无需 #[macro_export] 污染全局命名空间 */
pub(crate) use {log_error, log_info, log_warn};
