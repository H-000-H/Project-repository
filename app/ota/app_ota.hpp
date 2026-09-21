/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_ota.hpp
 * @brief OTA 升级任务: 双分区固件流下载 + 激活
 * @author H-000-H
 * @note  业务侧触发就一行: app::Ota::get_instance().request_ota(fw_len);
 * @note  app 启动流程必须补两件事(见 start.h), 否则双分区升级不成立:
 *        ① flash_stm32f4_init() + mini_boot_state_refresh() —— 不刷新则 current 恒为 image_0,
 *           跑在 image_1 时会覆盖自己;
 *        ② 自检通过后 mini_boot_confirm_ota() —— 不确认则下次复位被判"试运行超时"而回滚。
 */
#ifndef APP_OTA_APP_OTA_HPP_
#define APP_OTA_APP_OTA_HPP_

#include <cstdint>

struct device; /* 前向声明 C 结构体 */

namespace app
{

/** @brief OTA 升级任务 */
class Ota
{
public:
    /** @brief 获取 OTA 任务单例 */
    static Ota& get_instance();

    Ota(const Ota&) = delete;            /**< 禁用拷贝构造 */
    Ota& operator=(const Ota&) = delete; /**< 禁用拷贝赋值 */
    Ota(Ota&&) = delete;                 /**< 禁用移动构造 */
    Ota& operator=(Ota&&) = delete;      /**< 禁用移动赋值 */

    /** @brief 启动 OTA (ota_open + ota_rollback_open + 置启动标志) */
    void start_ota();

    /** @brief 停止 OTA (清启动标志) */
    void stop_ota();

    /**
     * @brief 设置固件总长度
     * @param[in] len uint32_t 固件镜像总字节数
     */
    void set_fw_len(uint32_t len);

    /**
     * @brief 业务侧入口: set_fw_len + start_ota
     * @param[in] len uint32_t 固件镜像总字节数
     */
    void request_ota(uint32_t len);

    /**
     * @brief 任务注册 (main 里调用一次)
     * @return bool 创建成功返回 true, 失败返回 false
     */
    bool thread_register();

private:
    Ota();  /**< 构造: 查找并打开 OTA 串口设备, 失败则保持未绑定 */
    ~Ota(); /**< 析构: 默认实现 */

    /**
     * @brief 任务体: 死循环执行 ota_step 并按 k_task_period_ms 让出
     * @param[in] param void* 线程参数 (未使用)
     */
    static void thread(void* param);

    /** @brief OTA 单步: 满足启动条件时流式下载 → 激活 → 复位 */
    static void ota_step();

    static constexpr unsigned int  k_task_priority = 14;    /**< mini-os: 数值越小越优先 */
    static constexpr std::uint32_t k_task_stack    = 1024;  /**< mini-os 线程栈 (字节) */
    static constexpr const char*   k_task_name     = "Ota_Task";
    static constexpr const char*   k_tag           = "Ota";
    static constexpr const char*   k_dev_label     = "ota"; /**< OTA 串口 label, 换口只改这里 */

    ::device* m_driver       = nullptr;
    uint32_t  m_fw_total_len = 0;
    bool      m_is_start     = false;
};

} // namespace app

#endif // APP_OTA_APP_OTA_HPP_
