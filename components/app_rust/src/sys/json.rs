/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file json.rs
 * @brief cJSON 的 safe 封装 (RAII: 文档析构自动 cJSON_Delete)
 * @note JsonNode 借用文档, 故不可能活得比文档久 —— 避免 use-after-free。
 */
use crate::sys::ffi::{
    cJSON_Delete, cJSON_GetErrorPtr, cJSON_GetNumberValue, cJSON_GetObjectItem, cJSON_GetStringValue, cJSON_IsNumber,
    cJSON_IsString, cJSON_Parse,
};
use core::ffi::{CStr, c_void};
use core::marker::PhantomData;

/// 已解析的 JSON 文档 (析构自动释放)
pub struct JsonDoc {
    raw: *mut c_void,
}

impl JsonDoc {
    /**
     * @brief 解析 JSON 文本
     * @param[in] text 以 '\0' 结尾的 JSON 文本
     * @return 成功返回文档; 失败返回 None
     */
    pub fn parse(text: &CStr) -> Option<Self> {
        let raw = unsafe { cJSON_Parse(text.as_ptr()) };
        (!raw.is_null()).then_some(Self { raw })
    }

    /**
     * @brief 取对象成员
     * @param[in] key 键名
     * @return 成员节点; 不存在返回 None
     */
    pub fn get(&self, key: &CStr) -> Option<JsonNode<'_>> {
        let raw = unsafe { cJSON_GetObjectItem(self.raw, key.as_ptr()) };
        (!raw.is_null()).then_some(JsonNode { raw, _owner: PhantomData })
    }
}

impl Drop for JsonDoc {
    fn drop(&mut self) {
        unsafe { cJSON_Delete(self.raw) };
    }
}

/// 文档内的节点 (生命周期受 JsonDoc 约束)
pub struct JsonNode<'a> {
    raw: *mut c_void,
    _owner: PhantomData<&'a JsonDoc>,
}

impl JsonNode<'_> {
    /// @brief 是否为字符串型
    /// @return 是字符串型返回 true
    pub fn is_string(&self) -> bool {
        unsafe { cJSON_IsString(self.raw) != 0 }
    }

    /// @brief 是否为数值型
    /// @return 是数值型返回 true
    pub fn is_number(&self) -> bool {
        unsafe { cJSON_IsNumber(self.raw) != 0 }
    }

    /// @brief 取字符串值
    /// @return 成功返回字符串; 非字符串返回 None
    pub fn as_str(&self) -> Option<&CStr> {
        if !self.is_string() {
            return None;
        }
        let p = unsafe { cJSON_GetStringValue(self.raw) };
        (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) })
    }

    /// @brief 取数值
    /// @return 成功返回数值; 非数值返回 None
    pub fn as_f64(&self) -> Option<f64> {
        self.is_number().then(|| unsafe { cJSON_GetNumberValue(self.raw) })
    }
}

/// 上一次解析失败的位置
/// @return 失败位置; 无失败记录返回 None
pub fn error_pos() -> Option<&'static CStr> {
    let p = unsafe { cJSON_GetErrorPtr() };
    (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) })
}
