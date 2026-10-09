#pragma once
#include "osal.h"
#include "driver.h"
#include "device.h"
#include "etl/optional.h"
#include "vfs-gpio.h"
#include "xtask.h"
#include <cstdint>
namespace Led
{
    class Led_Task
    {
    private:
        device * pdev=nullptr;
        int Led_Status=0;
        Led_Task()
        {
            this->pdev = device_find_by_label("led");
            CRITICAL_ASSERT(this->pdev != nullptr, "LED device 'led' not found in device tree");
            device_open(this->pdev, nullptr);
        }
        Led_Task(const Led_Task&) = delete;
        Led_Task& operator=(const Led_Task&) = delete;
        ~Led_Task()=default;
    public:
        static constexpr uint16_t Led_Period = 500; /**< 裸机协程周期 (ms) */
        etl::optional<int> Turn_On_Light();
        etl::optional<int> Turn_Off_Light();
        etl::optional<int> Switch_Status();
        etl::optional<int> Check_Status();
        static Led_Task& Get_Instance() { static Led_Task instance; return instance; }
    };

    /** @brief LED 协程任务入口 (裸机 xtask, 用 PT_DELAY 让出) */
    void led_task_entry(x_task* task);
}