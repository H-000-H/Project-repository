/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file ota.hpp
 * @brief OTA (Over-the-Air) update class declaration.
 * @author H-000-H
 * @note 
 * - 使用方法(入口 main 只注册任务; 固件长度与启动由业务侧按需触发):
 *   1. 注册任务(main 里调用一次): `ota.thread_register();`
 *      (可以直接换成os后端只要改注册函数换几个条件编译就行)
 *   2. 业务侧(命令解析/协议/按键)判断要升级后一行触发:
 *      `APP_Ota::Ota::get_instance().request_ota(fw_len);`
 *      —— 内部依次 set_fw_len → ota_open → ota_rollback_open → 置启动标志
 *   3. (可选) 校验备用分区镜像是否完好: `mini_boot_backup();`
 * - app 启动流程里必须补两件事(见 start.h), 否则双分区升级不成立:
 *   a) `flash_stm32f4_init();` + `mini_boot_state_refresh();`
 *      —— 注册 flash 后端并读回持久位; current 决定下载写入哪个分区,
 *      不刷新则 current 恒为 image_0, 跑在 image_1 时会覆盖自己
 *   b) 自检通过后 `mini_boot_confirm_ota();` —— 清 pending + trial,
 *      不确认则在下次复位时被 boot 判为"试运行超时"而回滚到旧分区, OTA 白做
 * - 下载目标由 mini-ota 内部算(flash_inactive_area_id = 非当前分区), 本类不用管
 */
#ifndef APP_OTA_H
#define APP_OTA_H
#include "xtask.h"
#include <cstdint>

struct device;/*前向声明 C 结构体*/

namespace APP_Ota{
    class Ota
    {
    public:
        static Ota& get_instance();/*获取单例实例*/
        Ota(const Ota&)= delete;/*禁用拷贝构造函数*/

        Ota operator = (const Ota&)=delete;/*禁用拷贝赋值运算符*/

        Ota (Ota&&) = delete;/*禁用移动构造函数*/
        Ota& operator = (Ota&&) = delete;/*禁用移动赋值运算符*/

        void start_ota();/*启动Ota(业务侧调用: 内部ota_open + ota_rollback_open + 置位)*/
        void stop_ota();/*停止Ota*/

        void set_fw_len(uint32_t len);/*设置固件总长度*/

        void request_ota(uint32_t len);/*业务侧一行触发: set_fw_len + start_ota*/

        bool thread_register();/*任务注册函数(main里调用一次), 成功返回true*/
    private:
        Ota();
        ~Ota()= default;/*默认析构函数*/

        static void thread(x_task* self);/*协程回调(返回 void, 经 PT_DELAY 让出)*/

        static constexpr unsigned int kPriority = 4;/*任务优先级*/
        static constexpr const char* kName = "Ota_Task";/*任务名称*/
        static constexpr const char* kTag = "Ota";/*任务标签*/
        static constexpr const char* kDevLabel = "ota_uart";/*OTA串口设备label(USART3: PB10/PB11), 换口只改这里*/
        ::device* ota_driver = nullptr;/*ota驱动设备指针*/
        uint32_t fw_total_len = 0;/*固件总长度(业务侧 request_ota/set_fw_len 注入)*/
        bool is_start = false;/*是否启用Ota(业务侧 start_ota/stop_ota 控制)*/
#ifndef CONFIG_XTASK_PREEMPT
        static x_task s_tcb;/*协调式任务控制块*/
#endif
    };
};
#endif
