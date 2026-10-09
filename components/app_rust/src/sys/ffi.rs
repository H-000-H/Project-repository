/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file ffi.rs
 * @brief 全部 C 符号声明
 * @note 上层 (device / task / log / json) 据此封装 safe API, 业务模块不直接碰本文件。
 */
use core::ffi::{c_char, c_int, c_void};

// status.h 的 ERR_PTR 编码基址 (extern 数据符号, 用 `&raw` 取地址)
unsafe extern "C" {
    pub static ERR_SECTION_BASE: c_char;
}

// system_init.h / driver.h — 启动时序
unsafe extern "C" {
    /// 板级外设 / 驱动注册 / 静态分配 (调度器启动前)
    pub fn mini_tree_pre_os_init();
    /// 创建框架任务 (EventBus 分发、下半部等)
    pub fn mini_tree_start_tasks();
    /// 释放全局中断
    pub fn system_init_complete();
    /// 注册全部编译期生成的驱动 probe/remove 函数
    pub fn board_register_all_drivers();
}

// device.h — 设备模型 / VFS (struct device 在本层是不透明指针)
unsafe extern "C" {
    /// @param[in] label DTS label (须以 '\0' 结尾)
    /// @return 设备实例; 未找到返回 NULL, 失败返回 ERR_PTR
    pub fn device_find_by_label(label: *const c_char) -> *mut c_void;
    /// @param[in] pdev 设备实例
    /// @param[in] arg 打开参数 (可为 NULL)
    /// @return MINI_OK 成功, 负数错误码失败
    pub fn device_open(pdev: *mut c_void, arg: *mut c_void) -> c_int;
    /// @param[in] pdev 设备实例
    /// @return MINI_OK 成功, 负数错误码失败
    pub fn device_close(pdev: *mut c_void) -> c_int;
    /// @param[in] pdev 设备实例
    /// @param[in] cmd 控制命令
    /// @param[in] arg 命令参数
    /// @param[in] arg_len 参数长度
    /// @param[in] timeout_ms 超时毫秒数
    /// @return MINI_OK 成功, 负数错误码失败
    pub fn device_ioctl(pdev: *mut c_void, cmd: c_int, arg: *mut c_void, arg_len: usize, timeout_ms: u32) -> c_int;
    /// @param[in] pdev 设备实例
    /// @return 设备名称
    pub fn device_get_name(pdev: *const c_void) -> *const c_char;
}

// task_manager.h / mini_time.h — 任务与时间
unsafe extern "C" {
    /// @param[in] name 任务名
    /// @param[in] stack_size 栈大小 (字节)
    /// @param[in] priority 优先级 (FreeRTOS 语义: 数值越大越高)
    /// @param[in] entry 任务入口
    /// @param[in] param 任务参数
    /// @param[in] core_id 核心号 (-1 = 任意)
    /// @return 任务句柄; 失败返回 NULL
    pub fn task_manager_create_task(
        name: *const c_char,
        stack_size: u32,
        priority: u32,
        entry: extern "C" fn(*mut c_void),
        param: *mut c_void,
        core_id: c_int,
    ) -> *mut c_void;
    /// 延时毫秒 (FreeRTOS 后端下让出调度器)
    pub fn mini_delay_ms(ms: u32);
}

// esp_log.h — mini_tree 的 CONFIG_SYS_LOG_USE_ESP=y 落到此处
unsafe extern "C" {
    /// @param[in] level 日志级别
    /// @param[in] tag 日志标签
    /// @param[in] format 格式串
    /// @param[in] arg format 的第一个可变参
    /// @note 原型为可变参; Rust 不支持 variadic 声明, 故按定参声明,
    ///       调用点恒传 ("%s", msg): Xtensa ABI 下前 4 个实参同走 a1..a4,
    ///       与普通调用约定一致。
    pub fn esp_log_write(level: c_int, tag: *const c_char, format: *const c_char, arg: *const c_char) -> ();
}

// cJSON.h — 只用函数式 API, 不复刻 struct cJSON 布局
unsafe extern "C" {
    /// @param[in] value 以 '\0' 结尾的 JSON 文本
    /// @return 根节点; 失败返回 NULL
    pub fn cJSON_Parse(value: *const c_char) -> *mut c_void;
    /// @param[in] item 节点 (析构由 JsonDoc 负责)
    pub fn cJSON_Delete(item: *mut c_void);
    /// @param[in] object 根对象
    /// @param[in] string 键名
    /// @return 节点; 不存在返回 NULL
    pub fn cJSON_GetObjectItem(object: *const c_void, string: *const c_char) -> *mut c_void;
    /// @return 解析失败位置; 无失败返回 NULL
    pub fn cJSON_GetErrorPtr() -> *const c_char;
    /// @param[in] item 节点
    /// @return 非 0 表示字符串型
    pub fn cJSON_IsString(item: *const c_void) -> c_int;
    /// @param[in] item 节点
    /// @return 非 0 表示数值型
    pub fn cJSON_IsNumber(item: *const c_void) -> c_int;
    /// @param[in] item 节点
    /// @return 字符串值; 非字符串返回 NULL
    pub fn cJSON_GetStringValue(item: *const c_void) -> *mut c_char;
    /// @param[in] item 节点
    /// @return 数值
    pub fn cJSON_GetNumberValue(item: *const c_void) -> f64;
}
