/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TIM HAL — STM32F4 实现 (LL 库直投)
 *
 * 支持: 基本定时 / PWM / 输入捕获 / 输出比较 / 编码器 / Hall 传感器。
 * 设计: 硬件直投, DTSI 厂商宏值零翻译透传给 LL 库。
 */
#include "hal_tim.h"
#include "stm32f4xx_ll_tim.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_cortex.h"
#include "stm32f4xx.h"
#include "interrupt.h"

/** TIM 下半部工作项 (fn/arg 由 VFS 层绑定), 供 interrupt_virtual_register 注册 */
struct bottom_half_work g_tim_bottom_half_work;

#ifndef HAL_TIM_NUM
#define HAL_TIM_NUM 2U
#endif

/*===========================================================================================================================================================*/
/*结构体和全局变量定义*/
/*===========================================================================================================================================================*/

/**
 * @brief 定时器驱动结构体
 * @note 用于存储定时器驱动函数的初始化和关闭函数
 */
typedef struct tim_driver
{
    int (*init)(void* tim_handle, void* cfg_ptr, hal_tim_device* pdev);
    int (*close)(void* tim_handle, hal_tim_device* pdev);
} tim_driver_t;

/**
 * @brief 配置GPIO的复用功能
 * @param gpio GPIO配置结构体
 * @return MINI_OK 成功, MINI_ERR_INVAL 参数错误
 */
COMPAT_STATIC_INLINE int hal_tim_config_af_pin(hal_tim_pin_config* gpio)
{
    if (!gpio || !gpio->af)
        return MINI_ERR_INVAL;
    GPIO_TypeDef* port = (GPIO_TypeDef*)gpio->port;
    LL_GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.Pin        = gpio->pin;
    GPIO_InitStruct.Mode       = gpio->mode;
    GPIO_InitStruct.Pull       = gpio->pull;
    GPIO_InitStruct.Alternate  = gpio->af;
    GPIO_InitStruct.Speed      = gpio->speed;
    GPIO_InitStruct.OutputType = gpio->output_type;
    LL_AHB1_GRP1_EnableClock(gpio->clk_bus);
    if (LL_GPIO_Init(port, &GPIO_InitStruct) != SUCCESS)
        return MINI_ERR_INVAL;

    return MINI_OK;
}

/**
 * @brief 最基本的定时器初始化
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
COMPAT_STATIC_INLINE int _init_base(void* tim_handle, void* cfg_ptr, hal_tim_device* pdev)
{
    /**< 此处空操作因为定时器寄存器已经在 open中映射了本函数仅仅站位符 */
    COMPAT_IGNORE_RESULT(cfg_ptr); COMPAT_IGNORE_RESULT(pdev); COMPAT_IGNORE_RESULT(tim_handle);    
    return MINI_OK;
}
/**
 * @brief 初始化编码器
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @note  st的编码器通道被固定为CH1和CH2
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
 static int _init_encoder(void* tim_handle, void* cfg_ptr, hal_tim_device* pdev) 
 {
    if(!pdev || !tim_handle || !cfg_ptr)
        return MINI_ERR_INVAL;  
 
     /**<编码器固定占用且必须同时配置 2 个物理引脚*/
    if(hal_tim_config_af_pin(&pdev->host->encoder_mode.pin[0]) != MINI_OK)
        return MINI_ERR_IO;
    if(hal_tim_config_af_pin(&pdev->host->encoder_mode.pin[1]) != MINI_OK)
        return MINI_ERR_IO;
 
    LL_TIM_ENCODER_InitTypeDef* ENCODER_HANDLE = (LL_TIM_ENCODER_InitTypeDef*)cfg_ptr;
    COMPAT_MEM_SET(ENCODER_HANDLE, 0, sizeof(LL_TIM_ENCODER_InitTypeDef));
    /**<A相 (Channel 1) 调理电路配置*/
    ENCODER_HANDLE->EncoderMode     = pdev->host->encoder_mode.config.hw_cfg.mode;/**< 编码器模式 */
    ENCODER_HANDLE->IC1ActiveInput  = pdev->host->encoder_mode.config.hw_cfg.ic1_active_input;/**< A相输入源 */
    ENCODER_HANDLE->IC1Filter       = pdev->host->encoder_mode.config.hw_cfg.ic1_filter;/**< A相滤波 */
    ENCODER_HANDLE->IC1Prescaler    = pdev->host->encoder_mode.config.hw_cfg.ic1_prescaler;/**< A相分频器 */
    ENCODER_HANDLE->IC1Polarity     = pdev->host->encoder_mode.config.hw_cfg.ic1_polarity;/**< A相极性 */
     
     /**<B相 (Channel 2) 调理电路配置*/
    ENCODER_HANDLE->IC2ActiveInput  = pdev->host->encoder_mode.config.hw_cfg.ic2_active_input;/**< B相输入源 */
    ENCODER_HANDLE->IC2Filter       = pdev->host->encoder_mode.config.hw_cfg.ic2_filter;/**< B相滤波 */
    ENCODER_HANDLE->IC2Prescaler    = pdev->host->encoder_mode.config.hw_cfg.ic2_prescaler;/**< B相分频器 */
    ENCODER_HANDLE->IC2Polarity     = pdev->host->encoder_mode.config.hw_cfg.ic2_polarity;/**< B相极性 */
    
    if(LL_TIM_ENCODER_Init((TIM_TypeDef*)tim_handle, ENCODER_HANDLE) != SUCCESS)
        return MINI_ERR_NODEV;
         
    return MINI_OK; 
}

/**
 * @brief 初始化输出比较
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
static int _init_oc(void* tim_handle, void* cfg_ptr, hal_tim_device* pdev)   
{ 
    if(!pdev || !tim_handle || !cfg_ptr)
        return MINI_ERR_INVAL;

    LL_TIM_OC_InitTypeDef* OC_INIT_HANDLE = (LL_TIM_OC_InitTypeDef*)cfg_ptr;
    uint32_t mask = pdev->host->active_chn_mask;

    for(int i = 0; i < HAL_OUTPUT_COMPARE_TIM_MAX_CHANNELS; i++)
    {
        /**<如果对应的通道位没有置 1，说明该通道未启用，直接跳过*/
        if (!(mask & (1 << i))) 
            continue;

        COMPAT_MEM_SET(OC_INIT_HANDLE, 0, sizeof(LL_TIM_OC_InitTypeDef));
        
        if(hal_tim_config_af_pin(&pdev->host->oc_mode.pin[i]) != MINI_OK)
            return MINI_ERR_IO;

        OC_INIT_HANDLE->CompareValue   = pdev->host->oc_mode.config[i].compare_value;
        OC_INIT_HANDLE->OCMode         = pdev->host->oc_mode.config[i].oc_mode;
        OC_INIT_HANDLE->OCIdleState    = pdev->host->oc_mode.config[i].oc_idle_state;  
        OC_INIT_HANDLE->OCNIdleState   = pdev->host->oc_mode.config[i].oc_n_idle_state; 
        OC_INIT_HANDLE->OCPolarity     = pdev->host->oc_mode.config[i].oc_polarity;    
        OC_INIT_HANDLE->OCNPolarity    = pdev->host->oc_mode.config[i].oc_n_polarity;  
        OC_INIT_HANDLE->OCState        = pdev->host->oc_mode.config[i].oc_state;
        OC_INIT_HANDLE->OCNState       = pdev->host->oc_mode.config[i].oc_n_state;

        if(LL_TIM_OC_Init((TIM_TypeDef*)tim_handle, pdev->host->oc_mode.channel[i].channel_id, OC_INIT_HANDLE) != SUCCESS)
            return MINI_ERR_NODEV;
    }
    return MINI_OK; 
}

/**
 * @brief 初始化输入捕获
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
 static int _init_ic(void* tim_handle, void* cfg_ptr, hal_tim_device* pdev)   
 { 
     if(!pdev || !tim_handle || !cfg_ptr)
         return MINI_ERR_INVAL;
 
     uint32_t mask = pdev->host->active_chn_mask;
     LL_TIM_IC_InitTypeDef* IC_INIT_HANDLE = (LL_TIM_IC_InitTypeDef*)cfg_ptr;
 
     for(int i = 0; i < HAL_INPUT_CAPTURE_TIM_MAX_CHANNELS; i++)
     {
         if (!(mask & (1 << i))) 
             continue;
 
         if(hal_tim_config_af_pin(&pdev->host->ic_mode.pin[i]) != MINI_OK)
             return MINI_ERR_IO;
 
         COMPAT_MEM_SET(IC_INIT_HANDLE, 0, sizeof(LL_TIM_IC_InitTypeDef));
 
         /**< 填充当前通道的专属输入调理配置 */
         IC_INIT_HANDLE->ICActiveInput = pdev->host->ic_mode.config[i].active_input;
         IC_INIT_HANDLE->ICFilter      = pdev->host->ic_mode.config[i].filter;
         IC_INIT_HANDLE->ICPolarity    = pdev->host->ic_mode.config[i].polarity;
         IC_INIT_HANDLE->ICPrescaler   = pdev->host->ic_mode.config[i].prescaler;
 
         /**< 写入芯片硬件寄存器 */
         uint32_t ch_id = pdev->host->ic_mode.channel[i].channel_id;
         if(LL_TIM_IC_Init((TIM_TypeDef*)tim_handle, ch_id, IC_INIT_HANDLE) != SUCCESS)
             return MINI_ERR_NODEV;
     }
 
     return MINI_OK;
 }

/**
 * @brief 初始化断路器与死区
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
static int _init_bdtr(void* tim_handle, void* cfg_ptr,hal_tim_device* pdev) 
{ 
    if (!pdev||!tim_handle||!cfg_ptr) 
        return MINI_ERR_INVAL;

    LL_TIM_BDTR_InitTypeDef* BDTR_INIT_HANDLE   = (LL_TIM_BDTR_InitTypeDef*)cfg_ptr;
    BDTR_INIT_HANDLE->AutomaticOutput           = pdev->host->bdtr.automatic_output;
    BDTR_INIT_HANDLE->BreakPolarity             = pdev->host->bdtr.break_polarity;
    BDTR_INIT_HANDLE->BreakState                = pdev->host->bdtr.break_state;
    BDTR_INIT_HANDLE->OSSIState                 = pdev->host->bdtr.ossi_state;
    BDTR_INIT_HANDLE->OSSRState                 = pdev->host->bdtr.ossr_state;
    BDTR_INIT_HANDLE->DeadTime                  = pdev->host->bdtr.dead_time;
    BDTR_INIT_HANDLE->LockLevel                 = pdev->host->bdtr.lock_level;
    if(LL_TIM_BDTR_Init((TIM_TypeDef*)tim_handle,BDTR_INIT_HANDLE)!=SUCCESS)
        return MINI_ERR_NODEV; 
    return MINI_OK; 
}

/**
 * @brief 初始化霍尔传感器
 * @param tim_handle 定时器句柄
 * @param cfg_ptr 配置指针
 * @param pdev 定时器设备指针
 * @note  st的霍尔传感器通道被固定为CH1和CH2和CH3
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
static int _init_hall(void* tim_handle, void* cfg_ptr,hal_tim_device* pdev)
{
    if (!pdev||!tim_handle||!cfg_ptr) 
        return MINI_ERR_INVAL;
    for(int i = 0; i < 3; i++)
    {
        if(hal_tim_config_af_pin(&pdev->host->hall_mode.phase_pins[i])!=MINI_OK)
            return MINI_ERR_IO;
    }
    LL_TIM_HALLSENSOR_InitTypeDef* HALL_INIT_HANDLE = (LL_TIM_HALLSENSOR_InitTypeDef*)cfg_ptr;
    HALL_INIT_HANDLE->CommutationDelay              = pdev->host->hall_mode.config.hall_commutation_delay_time;
    HALL_INIT_HANDLE->IC1Filter                     = pdev->host->hall_mode.config.hall_filter_time;
    HALL_INIT_HANDLE->IC1Polarity                   = pdev->host->hall_mode.config.hall_polarity;
    HALL_INIT_HANDLE->IC1Prescaler                  = pdev->host->hall_mode.config.hall_prescaler;
    if(LL_TIM_HALLSENSOR_Init(tim_handle,HALL_INIT_HANDLE)!=SUCCESS)
        return MINI_ERR_NODEV;
    return MINI_OK;
}

/**
 * @brief 关闭编码器
 * @param tim_handle 定时器句柄
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
 static int _close_encode(void* tim_handle, struct hal_tim_device* pdev)
 {
     if (!pdev || !tim_handle) 
         return MINI_ERR_INVAL;
         
     TIM_TypeDef* TIMx = (TIM_TypeDef*)tim_handle;
     
     /**< 瞬间定格计数 */
     LL_TIM_DisableCounter(TIMx);
     
     /**< 批量拉高复合掩码（CH1 | CH2） */
     uint32_t ch_compound = LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2;
     
     /**< 批量禁用两个物理通道 */
     LL_TIM_CC_DisableChannel(TIMx, ch_compound);
     
     /**< 批量斩断 CC1IE、CC2IE 中断以及可能存在的全局更新中断(UIE) */
     uint32_t dier_clear = (ch_compound >> 1) | LL_TIM_DIER_UIE;
     LL_TIM_WriteReg(TIMx, DIER, LL_TIM_ReadReg(TIMx, DIER) & ~dier_clear);
     
     /**< 批量擦除 CC1IF、CC2IF、CC1OF、CC2OF 和全局更新标志(UIF) */
     uint32_t sr_clear = (ch_compound >> 1) | ((ch_compound >> 1) << 8) | LL_TIM_SR_UIF;
     LL_TIM_WriteReg(TIMx, SR, ~sr_clear);
     
     /**< 退回通用时钟源，解绑正交状态机 */
     LL_TIM_SetSlaveMode(TIMx, LL_TIM_SLAVEMODE_DISABLED);
     return MINI_OK;
 }

/**
 * @brief 关闭最基本的定时器
 * @param tim_handle 定时器句柄
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
 COMPAT_STATIC_INLINE int _close_base(void* tim_handle, struct hal_tim_device* pdev)
 {
     if (!pdev || !tim_handle) 
         return MINI_ERR_INVAL;
 
    COMPAT_IGNORE_RESULT(pdev);
     TIM_TypeDef* TIMx = (TIM_TypeDef*)tim_handle;
 
     /**< 关闭全局更新中断 */
     LL_TIM_DisableIT_UPDATE(TIMx);
 
     /**< 清除全局更新标志(UIF) */
     LL_TIM_ClearFlag_UPDATE(TIMx);
 
     /**< 彻底关闭计数器时钟 */
     LL_TIM_DisableCounter(TIMx);
 
     return MINI_OK;
 }

 /**
  * @brief 关闭输出比较通道、清除中断并停止计数器
  * @param tim_handle 定时器寄存器基址指针
  * @param pdev 定时器设备指针
  * @return 成功返回 MINI_OK, 参数非法返回 MINI_ERR_INVAL
  */
 static int _close_oc(void* tim_handle, struct hal_tim_device* pdev)
 {
     if (!pdev || !tim_handle) 
         return MINI_ERR_INVAL;
 
     TIM_TypeDef* TIM_Handle = (TIM_TypeDef*)tim_handle;
     uint32_t mask = pdev->host->active_chn_mask;
 
     for(int i = 0; i < HAL_OUTPUT_COMPARE_TIM_MAX_CHANNELS; i++)
     {
         if (!(mask & (1 << i))) 
             continue;
 
         /**<从 DTS 里直接解析出来的 ch_id，现在已经是原生的硬件掩码了（如 0x00000004）*/
         uint32_t ch_id = pdev->host->oc_mode.channel[i].channel_id;
         
         /**<禁用通道*/
         LL_TIM_CC_DisableChannel(TIM_Handle, ch_id);
         /**<设置通道模式为强制无效*/
         LL_TIM_OC_SetMode(TIM_Handle, ch_id, LL_TIM_OCMODE_FORCED_INACTIVE);
 
         /**< 因为 ch_id 的物理位映射在硬件上与控制逻辑具备天然的一致性（利用标准接口清除） */
         LL_TIM_WriteReg(TIM_Handle, DIER, LL_TIM_ReadReg(TIM_Handle, DIER) & ~(ch_id >> 1)); 
         /**< 顺手抹掉该通道对应的中断状态位 */
         LL_TIM_WriteReg(TIM_Handle, SR, ~(ch_id >> 1)); 
     }
 
     /**<如果定时器是高级定时器，则关闭所有输出*/
     if (TIM_Handle == TIM1 || TIM_Handle == TIM8) 
         LL_TIM_DisableAllOutputs(TIM_Handle);
 
     /**<彻底关闭计数器时钟*/
     LL_TIM_DisableCounter(TIM_Handle);
     return MINI_OK;
 }

 /**
  * @brief 关闭输入捕获
  * @param tim_handle 定时器句柄
  * @param pdev 定时器设备指针
  * @return MINI_OK 成功, MINI_ERR_INVAL 失败
  */
 static int _close_ic(void* tim_handle, struct hal_tim_device* pdev)
 {
     if(!tim_handle || !pdev)
         return MINI_ERR_INVAL;
 
     TIM_TypeDef* TIM_Handle = (TIM_TypeDef*)tim_handle;
     uint32_t mask = pdev->host->active_chn_mask;
 
     for(int i = 0; i < HAL_INPUT_CAPTURE_TIM_MAX_CHANNELS; i++)
     {
         if (!(mask & (1 << i))) 
             continue;
 
         /**< ch_id 直接就是原生硬件掩码 */
         uint32_t ch_id = pdev->host->ic_mode.channel[i].channel_id;
         
         /**< 关闭物理输入捕获通道 */
         LL_TIM_CC_DisableChannel(TIM_Handle, ch_id);
 
         /**< 干掉该通道的中断使能 */
         LL_TIM_WriteReg(TIM_Handle, DIER, LL_TIM_ReadReg(TIM_Handle, DIER) & ~(ch_id >> 1));
         
         /**< 正常捕获标志(CCxIF) + 过捕获错误标志(CCxOF)同时清除 */
         uint32_t sr_clear_mask = (ch_id >> 1) | ((ch_id >> 1) << 8);
         LL_TIM_WriteReg(TIM_Handle, SR, ~sr_clear_mask);
     }
 
     LL_TIM_DisableCounter(TIM_Handle);
     return MINI_OK;
}  
/**
 * @brief 关闭断路器与死区
 * @param tim_handle 定时器句柄
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
 static int _close_bdtr(void* tim_handle, struct hal_tim_device* pdev)
 {
     if(!tim_handle || !pdev)
         return MINI_ERR_INVAL;
 
     TIM_TypeDef* TIM_Handle = (TIM_TypeDef*)tim_handle;
 
     /**<只有高级定时器（TIM1/TIM8等）才有 BDTR 寄存器*/
     if (TIM_Handle == TIM1 || TIM_Handle == TIM8) 
     {
        /**<安全起见，先断开高级定时器的主输出总闸 (MOE = 0)*/
         LL_TIM_DisableAllOutputs(TIM_Handle);
 
        /**< 禁用自动输出功能*/
        LL_TIM_DisableAutomaticOutput(TIM_Handle);

        /**< 关闭断路器（刹车）使能，引脚不再响应外部紧急信号 */
        LL_TIM_ConfigBRK(TIM_Handle, LL_TIM_BREAK_DISABLE);

        /**< 顺手清除可能残存的刹车中断标志位 */
        LL_TIM_ClearFlag_BRK(TIM_Handle);

        /**< 将整个 BDTR 寄存器恢复到芯片复位后的默认干净状态（即无死区、无刹车、无锁） */
        LL_TIM_BDTR_InitTypeDef BDTR_CLOSE_STRUCT;
        LL_TIM_BDTR_StructInit(&BDTR_CLOSE_STRUCT);
        LL_TIM_BDTR_Init(TIM_Handle, &BDTR_CLOSE_STRUCT);
     }
 
     return MINI_OK;
}

/**
 * @brief 关闭霍尔传感器
 * @param tim_handle 定时器句柄
 * @param pdev 定时器设备指针
 * @return MINI_OK 成功, MINI_ERR_INVAL 失败
 */
static int _close_hall(void* tim_handle, struct hal_tim_device* pdev)
{
    if(!tim_handle || !pdev)
        return MINI_ERR_INVAL;
        
    TIM_TypeDef* TIM_Handle = (TIM_TypeDef*)tim_handle;

    /**< 彻底关闭计数器时钟 */
    LL_TIM_DisableCounter(TIM_Handle);

    /**< 斩断被征用的 CH1 */
    LL_TIM_CC_DisableChannel(TIM_Handle, LL_TIM_CHANNEL_CH1);

    /**< 彻底清除换向相关的 CC1 中断、从模式触发(TRIG)中断使能 */
    uint32_t dier_clear = LL_TIM_DIER_CC1IE | LL_TIM_DIER_TIE;
    LL_TIM_WriteReg(TIM_Handle, DIER, LL_TIM_ReadReg(TIM_Handle, DIER) & ~dier_clear);
    
    /**< 清洗换向留下的一切历史标志（CC1IF, CC1OF, TIF） */
    uint32_t sr_clear = LL_TIM_SR_CC1IF | LL_TIM_SR_CC1OF | LL_TIM_SR_TIF;
    LL_TIM_WriteReg(TIM_Handle, SR, ~sr_clear);

    /**< 拔掉大门：解除 3 路引脚的强行异或绑定（XOR） */
    CLEAR_BIT(TIM_Handle->CR2, TIM_CR2_TI1S);
    
    /**< 接触主从换向联动状态 */
    LL_TIM_SetSlaveMode(TIM_Handle, LL_TIM_SLAVEMODE_DISABLED);

    return MINI_OK;
}

static const tim_driver_t tim_drivers[TIM_DRIVER_COUNT] = 
{
    [HAL_TIM_MODE_BASE]         = { _init_base      ,       _close_base     },
    [HAL_TIM_MODE_OC]           = { _init_oc        ,       _close_oc       },
    [HAL_TIM_MODE_IC]           = { _init_ic        ,       _close_ic       },
    [HAL_TIM_MODE_ENCODER]      = { _init_encoder   ,       _close_encode   },
    [HAL_TIM_MODE_HALLSENSOR]   = { _init_hall      ,       _close_hall     },
};
/*===========================================================================================================================================================*/
/*函数声明*/
/*===========================================================================================================================================================*/

/*===========================================================================================================================================================*/
/* 外部核心 HAL 接口实现 */
/*===========================================================================================================================================================*/

/**
 * @brief 初始化 TIM 设备对象 (绑定 host 与平台私有配置)
 * @param pdev 定时器设备指针
 * @param unique 平台私有配置指针
 * @param host 定时器 host 配置指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_device_init(hal_tim_device* pdev, hal_tim_platform_unique_config* unique, hal_tim_host_config* host)
{
    if (!pdev || !unique || !host)
        return MINI_ERR_INVAL;

    COMPAT_MEM_SET(pdev, 0, sizeof(*pdev));
    pdev->host   = host;
    pdev->unique = unique;
    return MINI_OK;
}

/**
 * @brief 释放 TIM 设备对象 (清空 host/unique 引用)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK
 */
int hal_tim_device_deinit(hal_tim_device* pdev)
{
    if (!pdev)
        return MINI_OK;

    pdev->host   = NULL;
    pdev->unique = NULL;
    return MINI_OK;
}

/**
 * @brief 打开 TIM (时钟/时基/模式驱动/BDTR/启动计数器)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL 或 MINI_ERR_NODEV
 */
int hal_tim_open(hal_tim_device *pdev)
{   
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    /* 1. 时钟使能 — clk_periph 来自 DTS (LL 宏值), 总线从地址判断 */
    if (pdev->host->clk_periph)
    {
        if ((uint32_t)tim_handle >= APB2PERIPH_BASE)
            LL_APB2_GRP1_EnableClock(pdev->host->clk_periph);
        else
            LL_APB1_GRP1_EnableClock(pdev->host->clk_periph);
    }

    /* 2. 清除更新标志, 防止初始化前残留中断 */
    LL_TIM_ClearFlag_UPDATE(tim_handle);

    /* <补齐遗漏：配置全局时基（Base Counter 骨架）> */
    LL_TIM_InitTypeDef INIT_HANDLE = {0};
    INIT_HANDLE.Autoreload           = pdev->host->base.autoreload;
    INIT_HANDLE.ClockDivision        = pdev->host->base.clock_division;
    INIT_HANDLE.CounterMode          = pdev->host->base.counter_mode;
    INIT_HANDLE.RepetitionCounter    = pdev->host->base.repetition_counter;
    INIT_HANDLE.Prescaler            = pdev->host->base.prescaler;
    
    /**< 真正灌入硬件寄存器 */
    if (LL_TIM_Init(tim_handle, &INIT_HANDLE) != SUCCESS)
        return MINI_ERR_NODEV;

    /* <路由并动态调用具体模式（OC/IC/Encoder/Hall）的专属驱动> */
    uint32_t mode = pdev->host->mode;
    if (mode >= TIM_DRIVER_COUNT || !tim_drivers[mode].init)
        return MINI_ERR_INVAL;

    /**< 查模式驱动表, 本地调用 (不在 pdev 上存函数指针) */
    int (*init_func)(void*, void*, hal_tim_device*) = tim_drivers[mode].init;

    /*<定义局部的临时结构体缓冲区传递给模式驱动，防止越界污染 private_cfg>*/
    uint64_t init_buffer[sizeof(LL_TIM_OC_InitTypeDef) / sizeof(uint64_t) + 1] = {0};
    
    if (init_func(tim_handle, (void*)init_buffer, pdev) != MINI_OK)
        return MINI_ERR_NODEV;

    /* <如果在外面配了 BDTR 且是高级定时器，在此处联动灌入> */
    if (tim_handle == TIM1 || tim_handle == TIM8)
    {
        uint64_t bdtr_buffer[sizeof(LL_TIM_BDTR_InitTypeDef) / sizeof(uint64_t) + 1] = {0};
        _init_bdtr(tim_handle, (void*)bdtr_buffer, pdev);
        LL_TIM_EnableAllOutputs(tim_handle); /**< 开启高级定时器主输出总闸 */
    }

    /* 3. DIER 中断使能 — int_mask 来自 DTS (如 LL_TIM_DIER_UIE) */
    if (pdev->host->int_mask)
        tim_handle->DIER = pdev->host->int_mask;

    /* <拉起计数器手刹，让时钟正式开跑> */
    LL_TIM_EnableCounter(tim_handle);

    return MINI_OK;
}

/**
 * @brief 关闭 TIM (卸载 BDTR 并调用模式 close 驱动)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_close(hal_tim_device *pdev)
{
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    uint32_t mode = pdev->host->mode;

    if (mode >= TIM_DRIVER_COUNT || !tim_drivers[mode].close)
        return MINI_ERR_INVAL;

    /* <如果是高级定时器，先卸载 BDTR 守护和关闭总闸，确保安全> */
    if (tim_handle == TIM1 || tim_handle == TIM8)
    {
        _close_bdtr(tim_handle, pdev);
    }

    /* <完美调用具体的 close 多态驱动关闭通道> */
    return tim_drivers[mode].close(tim_handle, pdev);
}

/**
 * @brief 安全获取定时器指定通道的 CCR 寄存器指针 (可写)
 * @param tim TIM 外设基址
 * @param channel 通道号 (1..4)
 * @return 对应 CCR 寄存器指针
 */
COMPAT_STATIC_INLINE __IO uint32_t* hal_tim_get_ccr_ptr(TIM_TypeDef* tim, uint32_t channel)
{
    /**<CCR1~CCR4 的寄存器地址在内存中是严格连续的，每个寄存器占用 4 字节*/
    return (__IO uint32_t*)((uintptr_t)&tim->CCR1 + ((channel - 1U) * 4U));
}

/**
 * @brief 安全获取定时器指定通道的 CCR 寄存器指针 (只读)
 * @param tim TIM 外设基址
 * @param channel 通道号 (1..4)
 * @return 对应 CCR 寄存器 const 指针
 */
COMPAT_STATIC_INLINE const __IO uint32_t* hal_tim_get_ccr_ptr_const(const TIM_TypeDef* tim, uint32_t channel)
{
    /**<CCR1~CCR4 的寄存器地址在内存中是严格连续的，每个寄存器占用 4 字节*/
    return (const __IO uint32_t*)((uintptr_t)&tim->CCR1 + ((channel - 1U) * 4U));
}


/* =========================================================================================================================================================== */
/* 基础属性配置与获取函数                                                                                                                                       */
/* =========================================================================================================================================================== */

/**
 * @brief 读取 TIM 当前计数值
 * @param pdev 定时器设备指针
 * @param value 输出计数值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_counter(const hal_tim_device *pdev, uint32_t *value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetCounter((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 设置 TIM 当前计数值
 * @param pdev 定时器设备指针
 * @param value 计数值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_set_counter(hal_tim_device *pdev, uint32_t value)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_SetCounter((TIM_TypeDef*)pdev->host->tim_handle, value);
    return MINI_OK;
}

/**
 * @brief 停止基本定时器 (关 UIE、清 UIF、停计数器)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_base_stop(hal_tim_device*pdev)
{
    if(!pdev || !pdev->host)
       return MINI_ERR_INVAL;

    _close_base((void*)pdev->host->tim_handle,pdev);
    return MINI_OK;
}

/**
 * @brief 恢复基本定时器计数 (resume 语义, 仅 EnableCounter)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_base_start(hal_tim_device*pdev)
{
    /** resume 语义: PSC/ARR/CounterMode 已由 hal_tim_open 灌入硬件, 此处只拉手刹 */
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    LL_TIM_EnableCounter(tim_handle);
    return MINI_OK;
}

/**
 * @brief 清除 TIM 更新中断标志 (UIF)
 * @param pdev 定时器设备指针
 * @return 标志存在并已清除返回 MINI_OK, 无标志返回 MINI_ERR_IO
 */
int hal_tim_clear_update_flag(hal_tim_device* pdev)
{
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;
    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if (LL_TIM_IsActiveFlag_UPDATE(tim_handle))
    {
        LL_TIM_ClearFlag_UPDATE(tim_handle);
        return MINI_OK;
    }
    return MINI_ERR_IO;
}

/**
 * @brief 设置 TIM 自动重装载值 (ARR)
 * @param pdev 定时器设备指针
 * @param value ARR 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_set_autoreload(hal_tim_device *pdev, uint32_t value)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_SetAutoReload((TIM_TypeDef*)pdev->host->tim_handle, value);
    return MINI_OK;
}

/**
 * @brief 读取 TIM 自动重装载值 (ARR)
 * @param pdev 定时器设备指针
 * @param value 输出 ARR 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_autoreload(const hal_tim_device *pdev, uint32_t *value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetAutoReload((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 设置 TIM 预分频值 (PSC)
 * @param pdev 定时器设备指针
 * @param value PSC 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_set_prescaler(hal_tim_device *pdev, uint32_t value)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_SetPrescaler((TIM_TypeDef*)pdev->host->tim_handle, value);
    return MINI_OK;
}

/**
 * @brief 读取 TIM 预分频值 (PSC)
 * @param pdev 定时器设备指针
 * @param value 输出 PSC 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_prescaler(const hal_tim_device *pdev, uint32_t *value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetPrescaler((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 设置 TIM 时钟分频 (CKD)
 * @param pdev 定时器设备指针
 * @param value CKD 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_set_clock_division(hal_tim_device *pdev, uint32_t value)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_SetClockDivision((TIM_TypeDef*)pdev->host->tim_handle, value);
    return MINI_OK;
}

/**
 * @brief 读取 TIM 时钟分频 (CKD)
 * @param pdev 定时器设备指针
 * @param value 输出 CKD 值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_clock_division(const hal_tim_device *pdev, uint32_t *value)
{
    if(!pdev || !pdev->host || !value)   
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetClockDivision((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 设置 TIM 计数模式
 * @param pdev 定时器设备指针
 * @param value LL 计数模式宏值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_set_counter_mode(hal_tim_device *pdev, uint32_t value)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_SetCounterMode((TIM_TypeDef*)pdev->host->tim_handle, value);
    return MINI_OK;
}

/**
 * @brief 读取 TIM 计数模式
 * @param pdev 定时器设备指针
 * @param value 输出计数模式
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_counter_mode(const hal_tim_device *pdev, uint32_t *value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetCounterMode((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 使能 TIM ARR 预装载
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_enable_arr_preload(hal_tim_device *pdev)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_EnableARRPreload((TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 关闭 TIM ARR 预装载
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_disable_arr_preload(hal_tim_device *pdev)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    LL_TIM_DisableARRPreload((TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}


/* =========================================================================================================================================================== */
/* 五大模式操作函数 — 供 VFS 层调用                                                                                                                              */
/* =========================================================================================================================================================== */

/**
 * @brief 更新 PWM 频率与占空比 (ARR 与 CCR)
 * @param pdev 定时器设备句柄
 * @param channel 目标通道号 (1..4)
 * @param frequency 自动重装载值 (frequency > 0 时更新)
 * @param duty 比较寄存器值 (duty > 0 时更新)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_pwm_update(hal_tim_device* pdev, uint32_t channel, uint32_t frequency, uint32_t duty)
{
    if(!pdev || !pdev->host || channel < 1U || channel > 4U)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    if(frequency > 0U)
    {
        /**< 更新自动重装载值 */
        LL_TIM_SetAutoReload(tim_handle, frequency);
    }

    if(duty > 0U)
    {
        *hal_tim_get_ccr_ptr(tim_handle, channel) = duty;
    }

    return MINI_OK;
}

/**
 * @brief 配置 TIM DIER 中断使能寄存器 (全量赋值)
 * @param pdev 定时器设备指针
 * @param interrupt_config DIER 位掩码
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_interrupt_config(hal_tim_device* pdev, uint32_t interrupt_config)
{
    if (!pdev || !pdev->host)
        return MINI_ERR_INVAL;
 
    TIM_TypeDef* tim = (TIM_TypeDef*)pdev->host->tim_handle;
    if (!tim)
        return MINI_ERR_INVAL;
 
    /**< 直接赋值以全面接管状态（支持使能开、也支持置零关）。如果只需单向置位，则改为 tim->DIER |= interrupt_config; */
    tim->DIER = interrupt_config;
    return MINI_OK;
}

/**
 * @brief 读取输入捕获通道 CCR 值
 * @param pdev 定时器设备指针
 * @param channel 通道号 (1..4)
 * @param value 输出捕获值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_capture_value(const hal_tim_device* pdev, uint32_t channel, uint32_t* value)
{
    if(!pdev || !pdev->host || !value || channel < 1U || channel > 4U)
        return MINI_ERR_INVAL;

    const TIM_TypeDef* tim_handle = (const TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    *value = *hal_tim_get_ccr_ptr_const(tim_handle, channel);
    return MINI_OK;
}

/**
 * @brief 读取编码器模式计数值 (同 CNT)
 * @param pdev 定时器设备指针
 * @param value 输出编码器计数值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_encoder_value(const hal_tim_device* pdev, uint32_t* value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    *value = LL_TIM_GetCounter((const TIM_TypeDef*)pdev->host->tim_handle);
    return MINI_OK;
}

/**
 * @brief 读取霍尔传感器 CH1 捕获值
 * @param pdev 定时器设备指针
 * @param value 输出捕获值
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_get_hall_value(const hal_tim_device* pdev, uint32_t* value)
{
    if(!pdev || !pdev->host || !value)
        return MINI_ERR_INVAL;

    const TIM_TypeDef* tim_handle = (const TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    *value = LL_TIM_IC_GetCaptureCH1(tim_handle);
    return MINI_OK;
}

/**
 * @brief 强制停止 TIM (停计数器, 高级定时器关 MOE)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_force_stop(hal_tim_device* pdev)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    LL_TIM_DisableCounter(tim_handle);

    /**< 替代了硬编码判断 TIM1/TIM8，使用 ST 标准外设宏自动兼容具有断路/高级输出的外设 */
#if defined(IS_TIM_BREAK_INSTANCE)
    if(IS_TIM_BREAK_INSTANCE(tim_handle))
    {
        LL_TIM_DisableAllOutputs(tim_handle);
    }
#endif

    return MINI_OK;
}

/**
 * @brief 启动编码器模式 (设模式、使能 CH1/CH2、启动计数)
 * @param pdev 定时器设备指针
 * @param encoder_mode 编码器模式 (1=X2 TI1, 2=X2 TI2, 4=X4 TI12)
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_encoder_start(hal_tim_device* pdev, uint32_t encoder_mode)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    if(encoder_mode > 0U)
    {
        uint32_t ll_mode;
        /**< 采用分支结构代替查表法，避免不可控的外部传参引发数组越界崩溃 */
        switch(encoder_mode)
        {
            case 1U: ll_mode = LL_TIM_ENCODERMODE_X2_TI1; break;
            case 2U: ll_mode = LL_TIM_ENCODERMODE_X2_TI2; break;
            case 4U: 
            default: ll_mode = LL_TIM_ENCODERMODE_X4_TI12; break; /**< 超出预期的统一回退到标准 4 倍频模式 */
        }
        LL_TIM_SetEncoderMode(tim_handle, ll_mode);
    }

    LL_TIM_CC_EnableChannel(tim_handle, pdev->host->encoder_mode.channel[0].channel_id |
                                        pdev->host->encoder_mode.channel[1].channel_id);
    LL_TIM_EnableCounter(tim_handle);

    return MINI_OK;
}

/**
 * @brief 启动霍尔传感器模式 (XOR/CH1 捕获/UPDATE 事件)
 * @param pdev 定时器设备指针
 * @return 成功返回 MINI_OK, 失败返回 MINI_ERR_INVAL
 */
int hal_tim_hall_start(hal_tim_device* pdev)
{
    if(!pdev || !pdev->host)
        return MINI_ERR_INVAL;

    TIM_TypeDef* tim_handle = (TIM_TypeDef*)pdev->host->tim_handle;
    if(!tim_handle)
        return MINI_ERR_INVAL;

    LL_TIM_IC_EnableXORCombination(tim_handle);
    LL_TIM_CC_EnableChannel(tim_handle, pdev->host->hall_mode.capture_channel.channel_id);
    LL_TIM_EnableIT_CC1(tim_handle);
    LL_TIM_GenerateEvent_UPDATE(tim_handle);
    LL_TIM_EnableCounter(tim_handle);

    return MINI_OK;
}

/* =========================================================================================================================================================== */
/* ISR 虚拟中断回调                                                                                                                                              */
/* =========================================================================================================================================================== */

/**
 * @brief TIM 虚拟中断上半部回调 (清除 Update 标志)
 * @param arg 定时器设备指针 (hal_tim_device*)
 * @param irq_num 虚拟中断号 (当前忽略)
 * @return MINI_IRQ_ENTRY_BOTTOM 需要下半部; MINI_IRQ_ENTRY_NOBOTTOM 不需要
 */
int hal_virtual_tim_irq_callback(void* arg, uint16_t irq_num)
{
    COMPAT_IGNORE_RESULT(irq_num);
    hal_tim_device* pdev = (hal_tim_device*)arg;

    if (!pdev || !pdev->host)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    /** 清除 Update 标志, 非 Update 中断是 spurious */
    if (hal_tim_clear_update_flag(pdev) != MINI_OK)
        return MINI_IRQ_ENTRY_NOBOTTOM;

    /** 需要下半部: VFS 层通过 g_tim_bottom_half_work 注册回调 */
    return MINI_IRQ_ENTRY_BOTTOM;
}
