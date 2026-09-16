/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file app_ota.hpp
 * @brief OTA 升级任务: 双分区固件流下载 + 激活
 * @author H-000-H
 * @note
 * - 使用方法(入口 main 只注册任务; 固件长度与启动由业务侧按需触发):
 *   1. 注册任务(main 里调用一次): `ota.ThreadRegister();`
 *   2. 业务侧(命令解析/协议/按键)判断要升级后一行触发:
 *      `app::Ota::GetInstance().RequestOta(fw_len);`
 *      —— 内部依次 SetFwLen → ota_open → ota_rollback_open → 置启动标志
 *   3. (可选) 校验备用分区镜像是否完好: `mini_boot_backup();`
 * - app 启动流程里必须补两件事(见 start.h), 否则双分区升级不成立:
 *   a) `flash_stm32f4_init();` + `mini_boot_state_refresh();`
 *      —— 注册 flash 后端并读回持久位; current 决定下载写入哪个分区,
 *      不刷新则 current 恒为 image_0, 跑在 image_1 时会覆盖自己
 *   b) 自检通过后 `mini_boot_confirm_ota();` —— 清 pending + trial,
 *      不确认则在下次复位时被 boot 判为"试运行超时"而回滚到旧分区, OTA 白做
 * - 下载目标由 mini-ota 内部算(flash_inactive_area_id = 非当前分区), 本类不用管
 */
#ifndef APP_OTA_APP_OTA_HPP_
#define APP_OTA_APP_OTA_HPP_

#include <cstdint>

struct device; /* 前向声明 C 结构体 */

namespace app
{

/**
 * @brief OTA 升级任务
 */
class Ota
{
public:
    static Ota& GetInstance();

    Ota(const Ota&) = delete;
    Ota& operator=(const Ota&) = delete;
    Ota(Ota&&) = delete;
    Ota& operator=(Ota&&) = delete;

    /** @brief 启动 OTA (内部 ota_open + ota_rollback_open + 置位) */
    void StartOta();
    /** @brief 停止 OTA */
    void StopOta();

    /** @brief 设置固件总长度 */
    void SetFwLen(uint32_t len);

    /** @brief 业务侧入口: SetFwLen + StartOta */
    void RequestOta(uint32_t len);

    /** @brief 任务注册 (main 里调用一次), 成功返回 true */
    bool ThreadRegister();

private:
    Ota();
    ~Ota();

    static void Thread(void* param);

    static void OtaStep();

    static constexpr unsigned int  kTaskPriority = 14;   /* mini-os: 数值越小越优先 (OTA 最不急) */
    static constexpr std::uint32_t kTaskStack    = 1024; /* mini-os 线程栈 (字节) */
    static constexpr const char*  kTaskName     = "Ota_Task";
    static constexpr const char*  kTag          = "Ota";
    /** @brief OTA 串口 client label (USART3: PB10/PB11), 换口只改这里 */
    static constexpr const char*  kDevLabel     = "ota";

    ::device* m_driver       = nullptr;
    uint32_t  m_fw_total_len = 0;
    bool      m_is_start     = false;
};

} // namespace app

#endif // APP_OTA_APP_OTA_HPP_
