/**
 * SPDX-License-Identifier: Apache-2.0
 * @brief STM32F1 GPIO 硬件直投实现（LL 库)
 * @note F1 差异：时钟走 APB2；LL_GPIO_PIN_x 为 32 位复合值(pin 用 uint32_t，LL 函数可直接用)；
 *       无 AFR 寄存器，复用靠 CRL/CRH + AFIO 重映射，set_af 仅切复用模式；
 *       EXTI 源端口走 AFIO->EXTICR。
 */
#include "hal_gpio.h"
#include "status.h"
#include "compiler_compat.h"
#include "interrupt.h"

#include "stm32f1xx_ll_gpio.h"
#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_exti.h"

/* =========================================================================
 * 纯硬件直投 fast path（零分支零查表）
 * ========================================================================= */

/**
 * @brief 快速设置 GPIO 输出电平 (BSRR 直写)
 * @param pdev GPIO 设备指针
 * @param level 0=低电平, 非 0=高电平
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_fast_set_level(hal_gpio_dev_t* pdev, int level)
{
    if (!pdev)
        return VFS_ERR_INVAL;

    GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
    /**< 零分支映射：通过位移运算同时兼容高电平(BSRR低16位 BS)与低电平(BSRR高16位 BR) */
    LL_GPIO_WriteReg(GPIOx, BSRR, ((pdev->pin >> GPIO_PIN_MASK_POS) & 0xFFFFU) << ((level == 0) << 4U));

    return VFS_OK;
}

/**
 * @brief 快速读取 GPIO 输入电平
 * @param pdev GPIO 设备指针
 * @param level_out 输出 0/1
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_fast_get_level(hal_gpio_dev_t* pdev, int* level_out)
{
    if (!pdev || !level_out)
        return VFS_ERR_INVAL;

    /**< LL_GPIO_IsInputPinSet 内部经 Pin>>GPIO_PIN_MASK_POS 提取位掩码，可直接收复合值 */
    *level_out = (LL_GPIO_IsInputPinSet((GPIO_TypeDef*)pdev->port, pdev->pin) != 0U);
    return VFS_OK;
}

/**
 * @brief 快速翻转 GPIO 输出电平
 * @param pdev GPIO 设备指针
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_fast_toggle(hal_gpio_dev_t* pdev)
{
    if (!pdev)
        return VFS_ERR_INVAL;

    LL_GPIO_TogglePin((GPIO_TypeDef*)pdev->port, pdev->pin);
    return VFS_OK;
}

/* =========================================================================
 * 纯硬件直投初始化与运行时控制 API
 * ========================================================================= */

/**
 * @brief GPIO 初始化 (时钟使能 + LL_GPIO_Init)
 * @param pdev GPIO 设备指针
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_init(hal_gpio_dev_t* pdev)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    /**< 开启对应的硬件时钟 (STM32F1: APB2 总线) */
    LL_APB2_GRP1_EnableClock(pdev->clk_bus);

    GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct;
    LL_GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.Pin        = pdev->pin;             /* LL_GPIO_PIN_x 复合值，LL_GPIO_Init 内部处理 */
    GPIO_InitStruct.Mode       = pdev->cfg.mode;
    GPIO_InitStruct.Pull       = pdev->cfg.pull;
    GPIO_InitStruct.Speed      = pdev->cfg.speed;
    GPIO_InitStruct.OutputType = pdev->cfg.output_type;

    if (LL_GPIO_Init(GPIOx, &GPIO_InitStruct) != SUCCESS)
        return VFS_ERR_INVAL;

    return VFS_OK;
}

/**
 * @brief 去初始化 GPIO 引脚 (恢复 deinit_mode/deinit_pull)
 * @param pdev GPIO 设备指针
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_deinit(hal_gpio_dev_t* pdev)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
    uint32_t rst_mode = pdev->cfg.deinit_mode ? pdev->cfg.deinit_mode : LL_GPIO_MODE_ANALOG;
    uint32_t rst_pull = pdev->cfg.deinit_pull ? pdev->cfg.deinit_pull : 0U; /* F1 无 LL_GPIO_PULL_NO，0=默认 */

    LL_GPIO_SetPinMode(GPIOx, pdev->pin, rst_mode);
    LL_GPIO_SetPinPull(GPIOx, pdev->pin, rst_pull);

    pdev->is_used = false;
    return VFS_OK;
}

/**
 * @brief 运行时设置 GPIO 引脚模式
 * @param pdev GPIO 设备指针
 * @param mode LL_GPIO_MODE_* 宏值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_mode(hal_gpio_dev_t* pdev, uint32_t mode)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    LL_GPIO_SetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin, mode);
    return VFS_OK;
}

/**
 * @brief 读取 GPIO 引脚当前模式
 * @param pdev GPIO 设备指针
 * @param mode 输出模式值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_get_mode(hal_gpio_dev_t* pdev, uint32_t* mode)
{
    if (!pdev || !pdev->is_used || !mode)
        return VFS_ERR_INVAL;

    *mode = LL_GPIO_GetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin);
    return VFS_OK;
}

/**
 * @brief 设置 GPIO 上下拉
 * @param pdev GPIO 设备指针
 * @param pull LL_GPIO_PULL_* 宏值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_pull(hal_gpio_dev_t* pdev, uint32_t pull)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    LL_GPIO_SetPinPull((GPIO_TypeDef*)pdev->port, pdev->pin, pull);
    return VFS_OK;
}

/**
 * @brief 读取 GPIO 上下拉配置
 * @param pdev GPIO 设备指针
 * @param pull 输出上下拉值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_get_pull(hal_gpio_dev_t* pdev, uint32_t* pull)
{
    if (!pdev || !pdev->is_used || !pull)
        return VFS_ERR_INVAL;

    *pull = LL_GPIO_GetPinPull((GPIO_TypeDef*)pdev->port, pdev->pin);
    return VFS_OK;
}

/**
 * @brief 设置 GPIO 输出速度
 * @param pdev GPIO 设备指针
 * @param speed LL_GPIO_SPEED_FREQ_* 宏值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_speed(hal_gpio_dev_t* pdev, uint32_t speed)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    LL_GPIO_SetPinSpeed((GPIO_TypeDef*)pdev->port, pdev->pin, speed);
    return VFS_OK;
}

/**
 * @brief 读取 GPIO 输出速度
 * @param pdev GPIO 设备指针
 * @param speed 输出速度值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_get_speed(hal_gpio_dev_t* pdev, uint32_t* speed)
{
    if (!pdev || !pdev->is_used || !speed)
        return VFS_ERR_INVAL;

    *speed = LL_GPIO_GetPinSpeed((GPIO_TypeDef*)pdev->port, pdev->pin);
    return VFS_OK;
}

/**
 * @brief 设置 GPIO 输出类型 (推挽/开漏)
 * @param pdev GPIO 设备指针
 * @param output_type LL_GPIO_OUTPUT_* 宏值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_output_type(hal_gpio_dev_t* pdev, uint32_t output_type)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    LL_GPIO_SetPinOutputType((GPIO_TypeDef*)pdev->port, pdev->pin, output_type);
    return VFS_OK;
}

/**
 * @brief 读取 GPIO 输出类型
 * @param pdev GPIO 设备指针
 * @param output_type 输出类型值
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_get_output_type(hal_gpio_dev_t* pdev, uint32_t* output_type)
{
    if (!pdev || !pdev->is_used || !output_type)
        return VFS_ERR_INVAL;

    *output_type = LL_GPIO_GetPinOutputType((GPIO_TypeDef*)pdev->port, pdev->pin);
    return VFS_OK;
}

/**
 * @brief 设置 GPIO 复用功能 (F1 无独立 AFR 寄存器, 复用靠 CRL/CRH CNF 位 + AFIO 重映射)
 * @note  F1 的复用功能编号对应具体外设重映射(AFIO->MAPR)，需结合外设使用；此处仅将引脚置为
 *        复用模式(CNF=0b10 复用推挽)，AFIO 重映射由具体外设驱动自行配置。
 * @param pdev GPIO 设备指针
 * @param af 复用功能编号 (F1 无寄存器直接写入, 保留参数仅做模式切换)
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_af(hal_gpio_dev_t* pdev, uint32_t af)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    COMPAT_IGNORE_RESULT(af);

    LL_GPIO_SetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin, LL_GPIO_MODE_ALTERNATE);
    return VFS_OK;
}

/**
 * @brief 读取 GPIO 复用功能 (F1 无 AFR 寄存器, 返回 CNF 位以标识是否处于复用模式)
 * @param pdev GPIO 设备指针
 * @param af 输出复用功能编号
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_get_af(hal_gpio_dev_t* pdev, uint32_t* af)
{
    if (!pdev || !pdev->is_used || !af)
        return VFS_ERR_INVAL;

    uint32_t mode = LL_GPIO_GetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin);
    *af = ((mode & 0x08U) != 0U) ? LL_GPIO_MODE_ALTERNATE : 0U;
    return VFS_OK;
}

/**
 * @brief 设置 GPIO 复用功能并切换为复用模式
 * @param pdev GPIO 设备指针
 * @param af 复用功能编号
 * @return 成功返回 VFS_OK, 失败返回 VFS_ERR_INVAL
 */
int hal_gpio_set_af_mode(hal_gpio_dev_t* pdev, uint32_t af)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;

    /**< 置为复用模式 (CNF=0b10 复用推挽) */
    return hal_gpio_set_af(pdev, af);
}

/* =========================================================================
 * GPIO 中断 (EXTI) 使能/关闭 — 仅硬件配置，分发(IRQHandler/VIRQ)不在此处
 * ========================================================================= */

#define HAL_GPIO_EXTI_LINES 16U

/**
 * @brief 根据 EXTI 线号选择 NVIC 中断号 (F1: EXTI0_IRQn / EXTI9_5_IRQn / EXTI15_10_IRQn)
 * @param line EXTI 线号 (0..15)
 * @return 对应 IRQn
 */
static IRQn_Type hal_gpio_exti_irqn(uint32_t line)
{
    if (line <= 4U)
        return (IRQn_Type)(EXTI0_IRQn + (int)line);
    if (line <= 9U)
        return EXTI9_5_IRQn;
    return EXTI15_10_IRQn;
}

/**
 * @brief 使能硬件 GPIO 中断 (EXTI)
 * @note  仅完成 AFIO 源端口 + EXTI 触发/使能 + NVIC；分发(IRQHandler/VIRQ)由中断框架层负责，
 *        故此处不注册/派发任何中断。
 * @param pdev GPIO 设备指针
 * @return 成功返回 VFS_OK, 参数错误或 cfg.intr==0 返回 VFS_OK(不配置)
 */
int hal_gpio_irq_enable(hal_gpio_dev_t* pdev)
{
    if (!pdev || !pdev->is_used)
        return VFS_ERR_INVAL;
    if (pdev->cfg.intr == 0U)
        return VFS_OK; /* 未配置中断，不使能硬件 EXTI */

    uint32_t line = COMPAT_CTZ((pdev->pin >> GPIO_PIN_MASK_POS) & 0xFFFFU); /* 真实线号 0..15 */
    if (line >= HAL_GPIO_EXTI_LINES)
        return VFS_ERR_INVAL;

    /* 1. 使能 AFIO 时钟，选择 EXTI 源端口 (AFIO->EXTICR) */
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_AFIO);
    uint32_t port_idx = (uint32_t)((pdev->port - GPIOA_BASE) / 0x400U); /* GPIOA..G */
    if (port_idx > 6U)
        return VFS_ERR_INVAL;
    uint32_t exti_line = ((0xFU << (4U * (line & 3U))) << 16U) | (line >> 2U); /* LL_GPIO_AF_EXTI_LINEx 复合值 */
    LL_GPIO_AF_SetEXTISource(port_idx, exti_line);

    /* 2. 配置触发方式 (F1 的 cfg.intr 为 LL_EXTI_TRIGGER_* = 1 上升/2 下降/3 双边) */
    uint32_t line_mask = 1UL << line;
    if ((pdev->cfg.intr & LL_EXTI_TRIGGER_RISING) != 0U)
        LL_EXTI_EnableRisingTrig_0_31(line_mask);
    if ((pdev->cfg.intr & LL_EXTI_TRIGGER_FALLING) != 0U)
        LL_EXTI_EnableFallingTrig_0_31(line_mask);

    /* 3. 使能中断线 + NVIC */
    LL_EXTI_EnableIT_0_31(line_mask);
    NVIC_SetPriority(hal_gpio_exti_irqn(line), 5);
    NVIC_EnableIRQ(hal_gpio_exti_irqn(line));

    return VFS_OK;
}

/**
 * @brief 关闭该脚硬件 GPIO 中断路由
 * @param pdev GPIO 设备指针
 * @return 成功返回 VFS_OK, 参数错误返回 VFS_ERR_INVAL
 */
int hal_gpio_irq_disable(hal_gpio_dev_t* pdev)
{
    if (!pdev)
        return VFS_ERR_INVAL;

    uint32_t line = COMPAT_CTZ((pdev->pin >> GPIO_PIN_MASK_POS) & 0xFFFFU);
    if (line >= HAL_GPIO_EXTI_LINES)
        return VFS_ERR_INVAL;

    LL_EXTI_DisableIT_0_31(1UL << line);
    NVIC_DisableIRQ(hal_gpio_exti_irqn(line));

    return VFS_OK;
}
