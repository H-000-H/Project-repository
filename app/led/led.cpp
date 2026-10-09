#include "led.hpp"
#include "osal.h"
#include "vfs-gpio.h"
#include "xtask.h"
#include <etl/optional.h>
namespace Led
{
    etl::optional<int> Led_Task::Turn_On_Light()
    {
        struct vfs_gpio_arg arg;
        arg.level = 1;
        if(device_ioctl(pdev,GPIO_CMD_SET_LEVEL,&arg,sizeof(arg),0)!=VFS_OK)
            return etl::nullopt;

        return etl::make_optional(VFS_OK);
    }

    etl::optional<int> Led_Task::Turn_Off_Light()
    {
        struct vfs_gpio_arg arg;
        arg.level = 0;
        if(device_ioctl(pdev,GPIO_CMD_SET_LEVEL,&arg,sizeof(arg),0)!=VFS_OK)
            return etl::nullopt;

        return etl::make_optional(VFS_OK);
    }

    etl::optional<int> Led_Task::Switch_Status()
    {
        struct vfs_gpio_arg arg;
        if (device_ioctl(pdev, GPIO_CMD_TOGGLE, &arg, sizeof(arg), 0) != VFS_OK)
            return etl::nullopt;

        /* arg.level 经驱动回填, 反映当前引脚电平(0/1) */
        return etl::make_optional(arg.level);
    }

    etl::optional<int> Led_Task::Check_Status()
    {
        struct vfs_gpio_arg arg;
        if (device_ioctl(pdev, GPIO_CMD_GET_LEVEL, &arg, sizeof(arg), 0) != VFS_OK)
            return etl::nullopt;
        Led_Status = arg.level;
        return etl::make_optional(VFS_OK);
    }

    void led_task_entry(x_task* task)
    {
        PT_BEGIN(task);

        Led_Task::Get_Instance().Turn_On_Light();

        for (;;)
        {
            Led_Task::Get_Instance().Switch_Status();
            PT_DELAY(task, Led_Task::Led_Period);
        }

        PT_END(task);
    }
}