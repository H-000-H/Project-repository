/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file device.rs
 * @brief 设备 / VFS 的 safe 封装层
 */
use crate::sys::ffi::{
    ERR_SECTION_BASE, device_close, device_find_by_label, device_get_name, device_ioctl, device_open,
};
use core::ffi::{CStr, c_int, c_void};
use core::mem::size_of;

/// status.h: 成功
pub const MINI_OK: c_int = 0;
/// status.h: 设备不存在
pub const MINI_ERR_NODEV: c_int = -11;

/// 是否为 status.h 的 ERR_PTR 编码
/// @param[in] ptr 待判断指针
/// @return 是错误指针返回 true
fn is_err_rust(ptr: *const c_void) -> bool {
    let base = &raw const ERR_SECTION_BASE as usize;
    !ptr.is_null() && (ptr as usize) >= base
}

/// 已打开的设备句柄 (对应 C 的 struct device*)
#[derive(Clone, Copy)]
pub struct Device(*mut c_void);

impl Device {
    /// @brief 取裸指针 (作为任务参数传给 C 入口)
    /// @return 设备实例地址
    pub fn as_ptr(&self) -> *mut c_void {
        self.0
    }
}

/**
 * @brief 按 DTS label 查找并打开设备 (包装 device_find_by_label + device_open)
 * @param[in] label DTS label (须以 '\0' 结尾)
 * @return 成功返回句柄
 * @retval Err(MINI_ERR_NODEV) 设备不存在或查找失败
 * @retval Err(其它负码) device_open 失败
 */
pub fn device_open_rust(label: &CStr) -> Result<Device, c_int> {
    let raw = unsafe { device_find_by_label(label.as_ptr()) };
    if raw.is_null() || is_err_rust(raw.cast_const()) {
        return Err(MINI_ERR_NODEV);
    }
    match unsafe { device_open(raw, core::ptr::null_mut()) } {
        MINI_OK => Ok(Device(raw)),
        err => Err(err),
    }
}

/**
 * @brief 发送带参数的控制命令 (包装 device_ioctl)
 * @param[in] dev 设备句柄
 * @param[in] cmd 控制命令
 * @param[in] arg 参数包, 长度按 T 推导
 * @param[in] timeout_ms 超时毫秒数
 * @return 成功返回 Ok(()); 参数可经 ioctl 回填
 */
pub fn device_ioctl_rust<T>(dev: &Device, cmd: c_int, arg: &mut T, timeout_ms: u32) -> Result<(), c_int> {
    let ret = unsafe { device_ioctl(dev.as_ptr(), cmd, (arg as *mut T).cast(), size_of::<T>(), timeout_ms) };
    if ret == MINI_OK { Ok(()) } else { Err(ret) }
}

/**
 * @brief 发送无参数的控制命令 (包装 device_ioctl)
 * @param[in] dev 设备句柄
 * @param[in] cmd 控制命令
 * @param[in] timeout_ms 超时毫秒数
 * @return 成功返回 Ok(())
 */
pub fn device_ioctl_none_rust(dev: &Device, cmd: c_int, timeout_ms: u32) -> Result<(), c_int> {
    let ret = unsafe { device_ioctl(dev.as_ptr(), cmd, core::ptr::null_mut(), 0, timeout_ms) };
    if ret == MINI_OK { Ok(()) } else { Err(ret) }
}

/**
 * @brief 取设备名 (包装 device_get_name)
 * @param[in] dev 设备句柄
 * @return 设备名称字符串
 */
pub fn device_get_name_rust(dev: &Device) -> &CStr {
    unsafe { CStr::from_ptr(device_get_name(dev.as_ptr())) }
}

/**
 * @brief 关闭设备 (包装 device_close)
 * @param[in] dev 设备句柄
 * @return 成功返回 Ok(())
 */
pub fn device_close_rust(dev: &Device) -> Result<(), c_int> {
    match unsafe { device_close(dev.as_ptr()) } {
        MINI_OK => Ok(()),
        err => Err(err),
    }
}

/**
 * @brief 由裸指针还原句柄 (供 C 任务入口回收包装)
 * @param[in] raw 设备实例地址
 * @return 设备句柄
 * @note 调用方须保证 raw 来自 device_open_rust
 */
pub unsafe fn device_from_ptr_rust(raw: *mut c_void) -> Device {
    Device(raw)
}