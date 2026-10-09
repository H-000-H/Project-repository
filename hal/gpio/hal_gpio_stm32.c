/**
 * SPDX-License-Identifier: Apache-2.0
 * @brief STM32 GPIO 硬件直投实现
 * @note 设计理念：尽量使用ll库函数，减少代码量，提高代码可读性，减少错误率，不是不得已不使用寄存器操作
 */
 #include "hal_gpio.h"
 #include "status.h"
 #include "compiler_compat.h"
 #include "interrupt.h"
 #include "stm32f4xx_hal.h"
 #include "stm32f4xx_ll_gpio.h"
 #include "stm32f4xx_ll_bus.h"
 #include "stm32f4xx_ll_exti.h"
 
/**
 * @brief 快速设置 GPIO 输出电平 (BSRR 直写)
 * @param pdev GPIO 设备指针
 * @param level 0=低电平, 非 0=高电平
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_fast_set_level(hal_gpio_dev_t* pdev, int level)
 {
     if (!pdev)
         return MINI_ERR_INVAL;
 
     GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
     /**< 零分支映射：通过位移运算同时兼容高电平(BSRR低16位)与低电平(BSRR高16位) */
     LL_GPIO_WriteReg(GPIOx, BSRR, pdev->pin << ((level == 0) << 4U));
 
     return MINI_OK;
 }
 
/**
 * @brief 快速读取 GPIO 输入电平
 * @param pdev GPIO 设备指针
 * @param level_out 输出 0/1
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_fast_get_level(hal_gpio_dev_t* pdev, int *level_out)
 {
     if (!pdev || !level_out)
         return MINI_ERR_INVAL;
 
     /**< 归一化为标准的 1 或 0 返回给上层 */
     *level_out = (LL_GPIO_IsInputPinSet((GPIO_TypeDef*)pdev->port, pdev->pin) != 0U);
     return MINI_OK;
 }
 
/**
 * @brief 快速翻转 GPIO 输出电平
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_fast_toggle(hal_gpio_dev_t* pdev)
 {
     if (!pdev)
         return MINI_ERR_INVAL;
 
     LL_GPIO_TogglePin((GPIO_TypeDef*)pdev->port, pdev->pin);
     return MINI_OK;
 }
 
 /* =========================================================================
  * 纯硬件直投初始化与运行时控制 API
  * ========================================================================= */
 
/**
 * @brief GPIO 初始化 (时钟使能 + LL_GPIO_Init)
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_init(hal_gpio_dev_t* pdev)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     /**< 开启对应的硬件时钟 */
     LL_AHB1_GRP1_EnableClock(pdev->clk_bus);
 
     GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
     LL_GPIO_InitTypeDef GPIO_InitStruct;
     LL_GPIO_StructInit(&GPIO_InitStruct);
 
     GPIO_InitStruct.Pin        = pdev->pin;
     GPIO_InitStruct.Mode       = pdev->cfg.mode;
     GPIO_InitStruct.Pull       = pdev->cfg.pull;
     GPIO_InitStruct.Speed      = pdev->cfg.speed;
     GPIO_InitStruct.OutputType = pdev->cfg.output_type;
     GPIO_InitStruct.Alternate  = pdev->cfg.af;
 
    if (LL_GPIO_Init(GPIOx, &GPIO_InitStruct) != SUCCESS)
         return MINI_ERR_INVAL;
    pdev->is_used = true;
    return MINI_OK;
 }
 
/**
 * @brief 去初始化 GPIO 引脚 (恢复 deinit_mode/deinit_pull)
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_deinit(hal_gpio_dev_t* pdev)
 {
    if (!pdev || !pdev->is_used)
        return MINI_ERR_INVAL;
    GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
    uint32_t rst_mode = pdev->cfg.deinit_mode ? pdev->cfg.deinit_mode : LL_GPIO_MODE_ANALOG;
    uint32_t rst_pull = pdev->cfg.deinit_pull ? pdev->cfg.deinit_pull : LL_GPIO_PULL_NO;
    LL_GPIO_SetPinMode(GPIOx, pdev->pin, rst_mode);
    LL_GPIO_SetPinPull(GPIOx, pdev->pin, rst_pull);
    pdev->is_used = false;
     return MINI_OK;
 }
 
/**
 * @brief 运行时设置 GPIO 引脚模式
 * @param pdev GPIO 设备指针
 * @param mode LL_GPIO 模式宏值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_gpio_set_mode(hal_gpio_dev_t* pdev, uint32_t mode)
{
    if (!pdev || !pdev->is_used)
        return MINI_ERR_INVAL;
 
    LL_GPIO_SetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin, mode);
    return MINI_OK;
}
 
/**
 * @brief 读取 GPIO 引脚当前模式
 * @param pdev GPIO 设备指针
 * @param mode 输出模式值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_get_mode(hal_gpio_dev_t* pdev, uint32_t *mode)
 {
     if (!pdev || !pdev->is_used || !mode)
         return MINI_ERR_INVAL;
 
     *mode = LL_GPIO_GetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin);
     return MINI_OK;
 }
 
/**
 * @brief 设置 GPIO 上下拉
 * @param pdev GPIO 设备指针
 * @param pull LL_GPIO 上下拉宏值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_set_pull(hal_gpio_dev_t* pdev, uint32_t pull)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     LL_GPIO_SetPinPull((GPIO_TypeDef*)pdev->port, pdev->pin, pull);
     return MINI_OK;
 }
 
/**
 * @brief 读取 GPIO 上下拉配置
 * @param pdev GPIO 设备指针
 * @param pull 输出上下拉值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_get_pull(hal_gpio_dev_t* pdev, uint32_t *pull)
 {
     if (!pdev || !pdev->is_used || !pull)
         return MINI_ERR_INVAL;
 
     *pull = LL_GPIO_GetPinPull((GPIO_TypeDef*)pdev->port, pdev->pin);
     return MINI_OK;
 }
 
/**
 * @brief 设置 GPIO 输出速度
 * @param pdev GPIO 设备指针
 * @param speed LL_GPIO 速度宏值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_set_speed(hal_gpio_dev_t* pdev, uint32_t speed)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     LL_GPIO_SetPinSpeed((GPIO_TypeDef*)pdev->port, pdev->pin, speed);
     return MINI_OK;
 }
 
/**
 * @brief 读取 GPIO 输出速度
 * @param pdev GPIO 设备指针
 * @param speed 输出速度值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_get_speed(hal_gpio_dev_t* pdev, uint32_t *speed)
 {
     if (!pdev || !pdev->is_used || !speed)
         return MINI_ERR_INVAL;
 
     *speed = LL_GPIO_GetPinSpeed((GPIO_TypeDef*)pdev->port, pdev->pin);
     return MINI_OK;
 }
 
/**
 * @brief 设置 GPIO 输出类型 (推挽/开漏)
 * @param pdev GPIO 设备指针
 * @param output_type LL_GPIO 输出类型宏值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_set_output_type(hal_gpio_dev_t* pdev, uint32_t output_type)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     LL_GPIO_SetPinOutputType((GPIO_TypeDef*)pdev->port, pdev->pin, output_type);
     return MINI_OK;
 }
 
/**
 * @brief 读取 GPIO 输出类型
 * @param pdev GPIO 设备指针
 * @param output_type 输出类型值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_get_output_type(hal_gpio_dev_t* pdev, uint32_t *output_type)
 {
     if (!pdev || !pdev->is_used || !output_type)
         return MINI_ERR_INVAL;
 
     *output_type = LL_GPIO_GetPinOutputType((GPIO_TypeDef*)pdev->port, pdev->pin);
     return MINI_OK;
 }
 
/**
 * @brief 设置 GPIO 复用功能 (AFR 寄存器)
 * @param pdev GPIO 设备指针
 * @param af 复用功能编号 (0..15)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_set_af(hal_gpio_dev_t* pdev, uint32_t af)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
     uint32_t pin_pos = (uint32_t)MINI_CTZ(pdev->pin);
     uint32_t shift = (pin_pos & 0x07U) * 4U;
 
     /**< 清空目标引脚所在的4位AFR空间，并写入新的AF值 */
     GPIOx->AFR[pin_pos >> 3U] = (GPIOx->AFR[pin_pos >> 3U] & ~(0x0FU << shift)) | (af << shift);
 
     return MINI_OK;
 }
 
/**
 * @brief 读取 GPIO 复用功能 (AFR 寄存器)
 * @param pdev GPIO 设备指针
 * @param af 输出复用功能编号
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_get_af(hal_gpio_dev_t* pdev, uint32_t *af)
 {
     if (!pdev || !pdev->is_used || !af)
         return MINI_ERR_INVAL;
 
     GPIO_TypeDef* GPIOx = (GPIO_TypeDef*)pdev->port;
     uint32_t pin_pos = (uint32_t)MINI_CTZ(pdev->pin);
 
     /**< 零分支直读：右移并清空高位，提取出目标引脚对应的 4-bit AF 寄存器值 */
     *af = (GPIOx->AFR[pin_pos >> 3U] >> ((pin_pos & 0x07U) * 4U)) & 0x0FU;
 
     return MINI_OK;
 }
 
/**
 * @brief 设置 GPIO 复用功能并切换为 AF 模式
 * @param pdev GPIO 设备指针
 * @param af 复用功能编号
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
 int hal_gpio_set_af_mode(hal_gpio_dev_t* pdev, uint32_t af)
 {
     if (!pdev || !pdev->is_used)
         return MINI_ERR_INVAL;
 
     /**< 1. 直投配置 AFR 寄存器值 */
     if (hal_gpio_set_af(pdev, af) != MINI_OK)
         return MINI_ERR_INVAL;
     
     /**< 2. 将引脚工作模式切换为复用模式 */
     LL_GPIO_SetPinMode((GPIO_TypeDef*)pdev->port, pdev->pin, LL_GPIO_MODE_ALTERNATE);
     
     return MINI_OK;
 }

 /* =========================================================================
  * GPIO EXTI 中断 → VIRQ(gpio, virq_idx) 虚拟中断分发
  * ========================================================================= */

#define HAL_GPIO_EXTI_LINES 16U

static hal_gpio_dev_t* s_exti_devs[HAL_GPIO_EXTI_LINES];

/**
 * @brief 配置 EXTI 源端口 (SYSCFG_EXTICR 寄存器直写)
 * @param port GPIO 端口基地址
 * @param line EXTI 线号 (0..15)
 * @return 成功返回 MINI_OK, 端口无效返回 MINI_ERR_INVAL
 */
static int hal_gpio_exti_set_source(uintptr_t port, uint32_t line)
{
    uint32_t port_idx = (uint32_t)((port - GPIOA_BASE) / 0x400U);
    uint32_t reg_idx, shift;

    if (port_idx > 6U)  /* STM32F407: GPIOA..GPIOG */
        return MINI_ERR_INVAL;

    reg_idx = line >> 2U;
    shift   = (line & 0x03U) * 4U;
    SYSCFG->EXTICR[reg_idx] = (SYSCFG->EXTICR[reg_idx] & ~(0x0FU << shift)) | (port_idx << shift);
    return MINI_OK;
}

/**
 * @brief 根据 EXTI 线号选择 NVIC 中断号
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
 * @brief EXTI 中断统一分发 (清标志 + VIRQ 派发)
 * @param line EXTI 线号 (0..15)
 */
static void hal_gpio_exti_dispatch(uint32_t line)
{
    hal_gpio_dev_t* pdev;

    LL_EXTI_ClearFlag_0_31(1UL << line);
    pdev = s_exti_devs[line];
    if (pdev && pdev->is_used)
    {
        interrupt_virtual_dispatch((uint16_t)VIRQ(gpio, pdev->virq_idx));
    }
}

void EXTI0_IRQHandler(void)
{
    hal_gpio_exti_dispatch(0U);
}

void EXTI1_IRQHandler(void)
{
    hal_gpio_exti_dispatch(1U);
}

void EXTI2_IRQHandler(void)
{
    hal_gpio_exti_dispatch(2U);
}

void EXTI3_IRQHandler(void)
{
    hal_gpio_exti_dispatch(3U);
}

void EXTI4_IRQHandler(void)
{
    hal_gpio_exti_dispatch(4U);
}

void EXTI9_5_IRQHandler(void)
{
    for (uint32_t line = 5U; line <= 9U; line++)
    {
        if (LL_EXTI_IsActiveFlag_0_31(1UL << line))
            hal_gpio_exti_dispatch(line);
    }
}

void EXTI15_10_IRQHandler(void)
{
    for (uint32_t line = 10U; line <= 15U; line++)
    {
        if (LL_EXTI_IsActiveFlag_0_31(1UL << line))
            hal_gpio_exti_dispatch(line);
    }
}

/**
 * @brief 使能硬件 GPIO 中断 (EXTI) → 仅 interrupt_virtual_dispatch(VIRQ(gpio, virq_idx))
 * @note  cfg.intr 为 DTS gpio-intr 直投的 LL_EXTI_TRIGGER_* 宏值; 0 = 不配置中断
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK
 */
int hal_gpio_irq_enable(hal_gpio_dev_t* pdev)
{
    uint32_t line;
    IRQn_Type irqn;
    LL_EXTI_InitTypeDef exti_init;

    if (!pdev || !pdev->is_used)
        return MINI_ERR_INVAL;
    if (pdev->cfg.intr == 0U)
        return MINI_OK;
    if (pdev->virq_idx >= VIRTUAL_IRQ_BLOCK_SIZE)
        return MINI_ERR_INVAL;

    line = (uint32_t)MINI_CTZ(pdev->pin);
    if (line >= HAL_GPIO_EXTI_LINES)
        return MINI_ERR_INVAL;

    if (hal_gpio_exti_set_source(pdev->port, line) != MINI_OK)
        return MINI_ERR_INVAL;

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

    LL_EXTI_StructInit(&exti_init);
    exti_init.Line_0_31   = 1UL << line;
    exti_init.LineCommand = ENABLE;
    exti_init.Mode        = LL_EXTI_MODE_IT;
    exti_init.Trigger     = pdev->cfg.intr;
    LL_EXTI_Init(&exti_init);

    s_exti_devs[line] = pdev;

    irqn = hal_gpio_exti_irqn(line);
    NVIC_SetPriority(irqn, 5);
    NVIC_EnableIRQ(irqn);

    return MINI_OK;
}

/**
 * @brief 关闭该脚硬件 GPIO 中断路由
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK
 */
int hal_gpio_irq_disable(hal_gpio_dev_t* pdev)
{
    uint32_t line;

    if (!pdev)
        return MINI_ERR_INVAL;

    line = (uint32_t)MINI_CTZ(pdev->pin);
    if (line >= HAL_GPIO_EXTI_LINES)
        return MINI_ERR_INVAL;

    LL_EXTI_DisableIT_0_31(1UL << line);
    if (s_exti_devs[line] == pdev)
        s_exti_devs[line] = NULL;

    return MINI_OK;
 }