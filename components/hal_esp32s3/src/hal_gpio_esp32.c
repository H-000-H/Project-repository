/**
 * SPDX-License-Identifier: Apache-2.0
 * @brief ESP32 GPIO 硬件直投实现
 * @note 设计理念：尽量使用 ESP-IDF 高层 API，减少代码量，提高可读性，不是不得已不使用寄存器操作
 * @note ESP32 无端口基地址/时钟概念, port/clk_bus 不使用, pin 承载 SoC GPIO 编号
 * @note 中断: 硬件 ISR 只 interrupt_virtual_dispatch(VIRQ(gpio,*))，业务在虚拟上下半部
 */
#include "hal_gpio.h"
#include "interrupt.h"
#include "status.h"
#include "compiler_compat.h"
#include "driver/gpio.h"

int hal_gpio_fast_set_level(hal_gpio_dev_t* pdev, int level)
{
    if (!pdev)
        return VFS_ERR_INVAL;

    gpio_set_level((gpio_num_t)pdev->pin, level);
    return VFS_OK;
}

int hal_gpio_fast_get_level(hal_gpio_dev_t* pdev, int* level_out)
{
    if (!pdev || !level_out)
        return VFS_ERR_INVAL;

    *level_out = gpio_get_level((gpio_num_t)pdev->pin) ? 1 : 0;
    return VFS_OK;
}

int hal_gpio_fast_toggle(hal_gpio_dev_t* pdev)
{
    int cur;

    if (!pdev)
        return VFS_ERR_INVAL;

    cur = gpio_get_level((gpio_num_t)pdev->pin);
    gpio_set_level((gpio_num_t)pdev->pin, cur ? 0 : 1);
    return VFS_OK;
}

/* =========================================================================
 * 纯硬件直投初始化与运行时控制 API
 * ========================================================================= */

int hal_gpio_init(hal_gpio_dev_t* pdev)
{
    gpio_config_t io_conf;

    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    gpio_reset_pin((gpio_num_t)pdev->pin);

    io_conf.intr_type    = (gpio_int_type_t)pdev->cfg.intr;
    io_conf.mode         = (gpio_mode_t)pdev->cfg.mode;
    io_conf.pin_bit_mask = (1ULL << pdev->pin);
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    if (gpio_config(&io_conf) != ESP_OK)
        return VFS_ERR_IO;

    if (gpio_set_pull_mode((gpio_num_t)pdev->pin, (gpio_pull_mode_t)pdev->cfg.pull) != ESP_OK)
        return VFS_ERR_IO;

    pdev->is_used = true;
    return VFS_OK;
}

int hal_gpio_deinit(hal_gpio_dev_t* pdev)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    if (pdev->cfg.deinit_mode)
    {
        if (gpio_set_direction((gpio_num_t)pdev->pin, (gpio_mode_t)pdev->cfg.deinit_mode) != ESP_OK)
            return VFS_ERR_IO;
    }
    else
        gpio_reset_pin((gpio_num_t)pdev->pin);

    if (pdev->cfg.deinit_pull)
    {
        if (gpio_set_pull_mode((gpio_num_t)pdev->pin, (gpio_pull_mode_t)pdev->cfg.deinit_pull) != ESP_OK)
            return VFS_ERR_IO;
    }

    pdev->is_used = false;
    return VFS_OK;
}

int hal_gpio_set_mode(hal_gpio_dev_t* pdev, uint32_t mode)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    if (gpio_set_direction((gpio_num_t)pdev->pin, (gpio_mode_t)mode) != ESP_OK)
        return VFS_ERR_IO;
    return VFS_OK;
}

int hal_gpio_get_mode(hal_gpio_dev_t* pdev, uint32_t* mode)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(mode);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_set_pull(hal_gpio_dev_t* pdev, uint32_t pull)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    if (gpio_set_pull_mode((gpio_num_t)pdev->pin, (gpio_pull_mode_t)pull) != ESP_OK)
        return VFS_ERR_IO;
    return VFS_OK;
}

int hal_gpio_get_pull(hal_gpio_dev_t* pdev, uint32_t* pull)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(pull);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_set_speed(hal_gpio_dev_t* pdev, uint32_t speed)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(speed);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_get_speed(hal_gpio_dev_t* pdev, uint32_t* speed)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(speed);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_set_output_type(hal_gpio_dev_t* pdev, uint32_t output_type)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(output_type);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_get_output_type(hal_gpio_dev_t* pdev, uint32_t* output_type)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(output_type);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_set_af(hal_gpio_dev_t* pdev, uint32_t af)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(af);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_get_af(hal_gpio_dev_t* pdev, uint32_t* af)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(af);
    return VFS_ERR_NOTSUPP;
}

int hal_gpio_set_af_mode(hal_gpio_dev_t* pdev, uint32_t af)
{
    COMPAT_IGNORE_RESULT(pdev);
    COMPAT_IGNORE_RESULT(af);
    return VFS_ERR_NOTSUPP;
}

static void gpio_virq_isr(void* arg)
{
    uint16_t virq = (uint16_t)(uintptr_t)arg;

    interrupt_virtual_dispatch(virq);
}

int hal_gpio_irq_enable(hal_gpio_dev_t* pdev)
{
    esp_err_t err;
    uint16_t  virq;

    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;
    if (pdev->cfg.intr == (uint32_t)GPIO_INTR_DISABLE)
        return VFS_OK;
    if (pdev->virq_idx >= VIRTUAL_IRQ_BLOCK_SIZE)
        return VFS_ERR_INVAL;

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return VFS_ERR_IO;

    virq = (uint16_t)VIRQ(gpio, pdev->virq_idx);
    if (gpio_isr_handler_add((gpio_num_t)pdev->pin, gpio_virq_isr,
                             (void*)(uintptr_t)virq) != ESP_OK)
        return VFS_ERR_IO;
    return VFS_OK;
}

int hal_gpio_irq_disable(hal_gpio_dev_t* pdev)
{
    if (!pdev)
        return VFS_ERR_INVAL;
    if (pdev->cfg.intr == (uint32_t)GPIO_INTR_DISABLE)
        return VFS_OK;
    if (gpio_isr_handler_remove((gpio_num_t)pdev->pin) != ESP_OK)
        return VFS_ERR_IO;
    return VFS_OK;
}
