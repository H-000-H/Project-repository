#pragma once
#include "osal.h"
#include "driver.h"
#include "device.h"
#include "etl/optional.h"
#include "vfs-gpio.h"
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
        static constexpr uint16_t Led_Stack = 1024;
        static constexpr uint8_t Led_Priority = 8;
        static constexpr uint8_t Led_Core_id =-1;
        etl::optional<int> Turn_On_Light();
        etl::optional<int> Turn_Off_Light();
        etl::optional<int> Switch_Status();
        etl::optional<int> Check_Status();
        static Led_Task& Get_Instance() { static Led_Task instance; return instance; }
    };
}