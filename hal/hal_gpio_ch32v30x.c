/**
 *@copyright SPDX-License-Identifier: Apache-2.0
 *@file hal_gpio_ch32v30x.c
 *@brief CH32V307 GPIO HAL 强符号实现 (覆盖 mini_tree/hal/gpio/hal_gpio.c 的 weak 空桩)
 *@author H-000-H
 *@details
 *   分层约束: 平台层 (hal), 只依赖 WCH 标准外设库与 mini_tree hal 头, 不碰上层。
 *   直投约定 (对齐 hal_gpio.h WCH 注释):
 *   - mode / pull / speed / output_type / af 字段直接承载 WCH 宏值:
 *     mode = GPIOMode_TypeDef (GPIO_Mode_Out_PP / GPIO_Mode_IPU ...);
 *     pull 编码: bit2 = ODR 方向 (1=上拉, 0=下拉), 仅 IPD/IPU 模式生效;
 *     speed = GPIOSpeed_TypeDef (0=50MHz 默认);
 *   - CH32V307 为 F1 型 GPIO (CRL/CRH), 无 AFR 复用选择寄存器,
 *     set_af/set_af_mode 退化为按 af 承载的 GPIOMode 重配引脚 (复用重映射走
 *     GPIO_PinRemapConfig, 需板级在 DTS 初始化阶段自行调用);
 *   - get_* 从 CR 寄存器回读, 重建为 GPIOMode_TypeDef 风格值 (输出模式含速度位);
 *   - IRQ 路由走 mini_tree interrupt_virtual_dispatch, 本文件仅操作 EXTI 线路
 *     使能, 触发类型由上层经 virq 子系统配置, 这里返回 NOTSUPP 交由板级扩展。
 */

#include "ch32v30x_hal_common.h"
#include "hal_gpio.h"

static uint32_t s_nibble_to_mode(GPIO_TypeDef* port, uint32_t idx)
{
    uint32_t reg = (idx < 8U) ? port->CFGLR : port->CFGHR;
    uint32_t nib = (reg >> ((idx & 0x7U) << 2)) & 0xFU;
    uint32_t cnf = (nib >> 2) & 0x3U;
    uint32_t mode_bits = nib & 0x3U;

    if (mode_bits == 0U)
    {
        /* 输入模式 */
        if (cnf == 0U)
            return (uint32_t)GPIO_Mode_AIN;
        if (cnf == 1U)
            return (uint32_t)GPIO_Mode_IN_FLOATING;
        if ((port->INDR & ((uint32_t)1U << idx)) != 0U)
            return (uint32_t)GPIO_Mode_IPU;
        return (uint32_t)GPIO_Mode_IPD;
    }

    /* 输出/复用模式: 低 2 位为速度, 高 2 位为 CNF */
    uint32_t speed = mode_bits;
    switch (cnf)
    {
    case 0U:
        return (uint32_t)GPIO_Mode_Out_PP | speed;
    case 1U:
        return (uint32_t)GPIO_Mode_Out_OD | speed;
    case 2U:
        return (uint32_t)GPIO_Mode_AF_PP | speed;
    default:
        return (uint32_t)GPIO_Mode_AF_OD | speed;
    }
}

/** 读取指定引脚当前 CR nibble */
static int s_read_nibble(hal_gpio_dev_t* pdev, uint32_t* nib_out)
{
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    uint32_t idx = 0;
    int ret = ch32_mask_to_index(mask, &idx);
    if (ret != MINI_OK)
        return ret;
    uint32_t reg = (idx < 8U) ? port->CFGLR : port->CFGHR;
    *nib_out = (reg >> ((idx & 0x7U) << 2)) & 0xFU;
    return MINI_OK;
}

/** 按 mode 宏重配引脚 (mode 承载 GPIOMode_TypeDef, 输出模式速度位缺省 50MHz) */
static int s_apply_mode(hal_gpio_dev_t* pdev, uint32_t mode)
{
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    if ((port == NULL) || (mask == 0U) || ((mask & ~0xFFFFU) != 0U))
        return MINI_ERR_INVAL;
    return ch32_gpio_cfg_pin(pdev->port, mask, mode, pdev->cfg.speed, pdev->cfg.pull);
}

/* =============================================================================
 * 纯硬件直投 fast path (零分支零查表)
 * ============================================================================= */

/**
 * @brief 快速设置 GPIO 输出电平 (BSHR/BCR 直写)
 * @param pdev GPIO 设备指针
 * @param level 0=低电平, 非 0=高电平
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_fast_set_level(hal_gpio_dev_t* pdev, int level)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    if (level != 0)
        port->BSHR = mask;
    else
        port->BCR = mask;
    return MINI_OK;
}

/**
 * @brief 快速读取 GPIO 输入电平 (INDR 直读)
 * @param pdev GPIO 设备指针
 * @param level_out 输出 0/1
 * @return 成功返回 MINI_OK, 参数为空返回 MINI_ERR_INVAL
 */
int hal_gpio_fast_get_level(hal_gpio_dev_t* pdev, int* level_out)
{
    if ((pdev == NULL) || (level_out == NULL))
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    *level_out = ((port->INDR & mask) != 0U) ? 1 : 0;
    return MINI_OK;
}

/**
 * @brief 快速翻转 GPIO 输出电平 (回读 OUTDR 后取反写 BSHR/BCR)
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_fast_toggle(hal_gpio_dev_t* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    if ((port->OUTDR & mask) != 0U)
        port->BCR = mask;
    else
        port->BSHR = mask;
    return MINI_OK;
}

/* =============================================================================
 * 上层接口 (hal_gpio.h)
 * ============================================================================= */

/**
 * @brief GPIO 初始化 (端口时钟使能 + GPIO_Init 直投)
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_init(hal_gpio_dev_t* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    int ret = ch32_gpio_port_clk(pdev->port, pdev->clk_bus);
    if (ret != MINI_OK)
        return ret;
    uint32_t mode = (pdev->cfg.mode != 0U) ? pdev->cfg.mode : (uint32_t)GPIO_Mode_IN_FLOATING;
    ret = ch32_gpio_cfg_pin(pdev->port, ch32_pin_to_mask(pdev->pin), mode, pdev->cfg.speed,
                            pdev->cfg.pull);
    if (ret != MINI_OK)
        return ret;
    pdev->is_used = true;
    return MINI_OK;
}

/**
 * @brief 去初始化 GPIO 引脚 (恢复 deinit_mode, 缺省回落输入浮空)
 * @param pdev GPIO 设备指针
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_deinit(hal_gpio_dev_t* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    /* deinit_mode/deinit_pull 为 0 时回落到安全输入浮空态 */
    uint32_t mode =
        (pdev->cfg.deinit_mode != 0U) ? pdev->cfg.deinit_mode : (uint32_t)GPIO_Mode_IN_FLOATING;
    int ret = ch32_gpio_cfg_pin(pdev->port, ch32_pin_to_mask(pdev->pin), mode, pdev->cfg.speed,
                                pdev->cfg.deinit_pull);
    if (ret != MINI_OK)
        return ret;
    pdev->is_used = false;
    return MINI_OK;
}

/**
 * @brief 运行时设置 GPIO 引脚模式
 * @param pdev GPIO 设备指针
 * @param mode GPIOMode_TypeDef 宏值 (DTS 直投)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_set_mode(hal_gpio_dev_t* pdev, uint32_t mode)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    return s_apply_mode(pdev, mode);
}

/**
 * @brief 读取 GPIO 引脚当前模式 (CR 回读重建为 GPIOMode 风格值)
 * @param pdev GPIO 设备指针
 * @param mode 输出模式值 (输出模式含速度位)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_get_mode(hal_gpio_dev_t* pdev, uint32_t* mode)
{
    if ((pdev == NULL) || (mode == NULL))
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    uint32_t idx = 0;
    int ret = ch32_mask_to_index(mask, &idx);
    if (ret != MINI_OK)
        return ret;
    *mode = s_nibble_to_mode(port, idx);
    return MINI_OK;
}

/**
 * @brief 设置 GPIO 上下拉 (pull bit2 = ODR 方向, 1=上拉)
 * @param pdev GPIO 设备指针
 * @param pull 上下拉直投值 (仅 IPD/IPU 模式生效)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_set_pull(hal_gpio_dev_t* pdev, uint32_t pull)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    uint32_t idx = 0;
    int ret = ch32_mask_to_index(mask, &idx);
    if (ret != MINI_OK)
        return ret;
    /* 仅输入上下拉引脚可直接改 ODR; 其余模式需要先切到 IPD/IPU */
    uint32_t nib = 0;
    ret = s_read_nibble(pdev, &nib);
    if (ret != MINI_OK)
        return ret;
    if (((nib & 0xFU) == 0x8U) || (((nib >> 2) & 0x3U) == 0x2U))
    {
        if ((pull & 0x04U) != 0U)
            port->BSHR = mask;
        else
            port->BCR = mask;
        return MINI_OK;
    }
    uint32_t mode = ((pull & 0x04U) != 0U) ? (uint32_t)GPIO_Mode_IPU : (uint32_t)GPIO_Mode_IPD;
    return ch32_gpio_cfg_pin(pdev->port, mask, mode, pdev->cfg.speed, pull);
}

/**
 * @brief 读取 GPIO 上下拉配置 (回读 ODR 方向位)
 * @param pdev GPIO 设备指针
 * @param pull 输出上下拉值 (0x04=上拉, 0=下拉)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_get_pull(hal_gpio_dev_t* pdev, uint32_t* pull)
{
    if ((pdev == NULL) || (pull == NULL))
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)pdev->port;
    uint32_t mask = ch32_pin_to_mask(pdev->pin);
    uint32_t idx = 0;
    int ret = ch32_mask_to_index(mask, &idx);
    if (ret != MINI_OK)
        return ret;
    /* bit2 = ODR 方向 (1=上拉), 与 ch32_gpio_cfg_pin 的 pull 编码一致 */
    *pull = ((port->OUTDR & mask) != 0U) ? 0x04U : 0x00U;
    return MINI_OK;
}

/**
 * @brief 设置 GPIO 输出速度 (nibble 低 2 位, 输入模式无速度位直接返回)
 * @param pdev GPIO 设备指针
 * @param speed GPIOSpeed_TypeDef 宏值 (1=10MHz/2=2MHz/3=50MHz)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_set_speed(hal_gpio_dev_t* pdev, uint32_t speed)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    uint32_t nib = 0;
    int ret = s_read_nibble(pdev, &nib);
    if (ret != MINI_OK)
        return ret;
    if ((nib & 0x3U) == 0U)
    {
        /* 输入模式无速度位, 记录即可 */
        return MINI_OK;
    }
    /* 输出模式: 速度位在 nibble 低 2 位, 重建 mode 宏后重配 */
    uint32_t mode =
        s_nibble_to_mode((GPIO_TypeDef*)pdev->port, __builtin_ctz(ch32_pin_to_mask(pdev->pin)));
    mode = (mode & ~0x3U) | (speed & 0x3U);
    return ch32_gpio_cfg_pin(pdev->port, ch32_pin_to_mask(pdev->pin), mode, speed, pdev->cfg.pull);
}

/**
 * @brief 读取 GPIO 输出速度
 * @param pdev GPIO 设备指针
 * @param speed 输出速度值 (nibble 低 2 位)
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_get_speed(hal_gpio_dev_t* pdev, uint32_t* speed)
{
    if ((pdev == NULL) || (speed == NULL))
        return MINI_ERR_INVAL;
    uint32_t nib = 0;
    int ret = s_read_nibble(pdev, &nib);
    if (ret != MINI_OK)
        return ret;
    *speed = nib & 0x3U;
    return MINI_OK;
}

/**
 * @brief 设置 GPIO 输出类型 (推挽/开漏)
 * @note  CH32V307 无独立输出类型字段, 按 CNF 最低位切换 (0=推挽, 1=开漏);
 *        输入模式不属于输出类型语义, 返回 NOTSUPP。
 * @param pdev GPIO 设备指针
 * @param output_type 0=推挽, 非 0=开漏
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL, 输入模式返回 MINI_ERR_NOTSUPP
 */
int hal_gpio_set_output_type(hal_gpio_dev_t* pdev, uint32_t output_type)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    /* CH32V307 无独立输出类型字段: 按 CNF 最低位语义在推挽/开漏间切换 (1=开漏) */
    uint32_t nib = 0;
    int ret = s_read_nibble(pdev, &nib);
    if (ret != MINI_OK)
        return ret;
    uint32_t idx = __builtin_ctz(ch32_pin_to_mask(pdev->pin));
    uint32_t mode = s_nibble_to_mode((GPIO_TypeDef*)pdev->port, idx);
    uint32_t want_od = (output_type != 0U) ? 1U : 0U;
    uint32_t cnf = ((nib >> 2) & 0x3U) & ~0x1U;
    cnf |= want_od;
    /* 重建枚举: 输入/输出分类 + CNF + 速度 */
    uint32_t speed = nib & 0x3U;
    if (speed == 0U)
    {
        /* 输入模式切上下拉/模拟不属于输出类型语义, 不支持 */
        return MINI_ERR_NOTSUPP;
    }
    static const uint32_t s_out_base[4] = {
        (uint32_t)GPIO_Mode_Out_PP,
        (uint32_t)GPIO_Mode_Out_OD,
        (uint32_t)GPIO_Mode_AF_PP,
        (uint32_t)GPIO_Mode_AF_OD,
    };
    mode = s_out_base[cnf] | speed;
    return ch32_gpio_cfg_pin(pdev->port, ch32_pin_to_mask(pdev->pin), mode, speed, pdev->cfg.pull);
}

/**
 * @brief 读取 GPIO 输出类型 (CNF bit0 = 1 视为开漏)
 * @param pdev GPIO 设备指针
 * @param output_type 输出 0=推挽, 1=开漏
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_get_output_type(hal_gpio_dev_t* pdev, uint32_t* output_type)
{
    if ((pdev == NULL) || (output_type == NULL))
        return MINI_ERR_INVAL;
    uint32_t nib = 0;
    int ret = s_read_nibble(pdev, &nib);
    if (ret != MINI_OK)
        return ret;
    /* CNF bit0 = 1 视为开漏 */
    *output_type = ((nib >> 2) & 0x1U);
    return MINI_OK;
}

/**
 * @brief 设置 GPIO 复用功能
 * @note  CH32V307 无 AFR 寄存器, af 字段承载 GPIOMode_TypeDef (如 GPIO_Mode_AF_PP),
 *        直投重配引脚; 复用重映射 (GPIO_PinRemapConfig) 由板级自行配置。
 * @param pdev GPIO 设备指针
 * @param af GPIOMode_TypeDef 宏值 (0 缺省复用推挽)
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_set_af(hal_gpio_dev_t* pdev, uint32_t af)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    /* CH32V307 无 AFR: af 字段承载 GPIOMode_TypeDef (如 GPIO_Mode_AF_PP), 直投重配 */
    uint32_t mode = (af != 0U) ? af : (uint32_t)GPIO_Mode_AF_PP;
    return ch32_gpio_cfg_pin(pdev->port, ch32_pin_to_mask(pdev->pin), mode, pdev->cfg.speed,
                             pdev->cfg.pull);
}

/**
 * @brief 读取 GPIO 复用功能 (回读当前模式宏作为 af 语义值, 与 set_af 对称)
 * @param pdev GPIO 设备指针
 * @param af 输出当前模式宏值
 * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
 */
int hal_gpio_get_af(hal_gpio_dev_t* pdev, uint32_t* af)
{
    if ((pdev == NULL) || (af == NULL))
        return MINI_ERR_INVAL;
    /* 回读当前模式宏作为 af 语义值 (与 set_af 的承载约定对称) */
    return hal_gpio_get_mode(pdev, af);
}

/**
 * @brief 设置复用功能并切换为复用模式 (F1 型引脚无独立切换步骤, 与 set_af 等价)
 * @param pdev GPIO 设备指针
 * @param af GPIOMode_TypeDef 宏值
 * @return 成功返回 MINI_OK, pdev 为空返回 MINI_ERR_INVAL
 */
int hal_gpio_set_af_mode(hal_gpio_dev_t* pdev, uint32_t af)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    /* 自动切复用: 与 set_af 等价 (F1 型引脚无独立模式切换步骤) */
    return hal_gpio_set_af(pdev, af);
}

/* =============================================================================
 * GPIO 中断 (EXTI) 使能/关闭 — 分发(中断框架/虚拟中断)不在此处, 本板交由板级扩展
 * ============================================================================= */

/**
 * @brief 使能硬件 GPIO 中断 (EXTI)
 * @note  EXTI 触发类型/虚拟中断分发由板级经 interrupt_virtual_register 配置,
 *        本文件不持有触发极性信息, 返回 NOTSUPP 交由板级扩展实现。
 * @param pdev GPIO 设备指针
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP (pdev 为空返回 MINI_ERR_INVAL)
 */
int hal_gpio_irq_enable(hal_gpio_dev_t* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    return MINI_ERR_NOTSUPP;
}

/**
 * @brief 关闭该脚硬件 GPIO 中断路由 (本板未实现, 与 irq_enable 对称)
 * @param pdev GPIO 设备指针
 * @return 本板未实现, 返回 MINI_ERR_NOTSUPP (pdev 为空返回 MINI_ERR_INVAL)
 */
int hal_gpio_irq_disable(hal_gpio_dev_t* pdev)
{
    if (pdev == NULL)
        return MINI_ERR_INVAL;
    return MINI_ERR_NOTSUPP;
}
