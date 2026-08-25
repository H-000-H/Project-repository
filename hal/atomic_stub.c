/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CH32V307 libatomic 兼容实现 — WCH 工具链 (riscv-wch-elf 12.2.0) 未提供 libatomic.a。
 */
#include <stdint.h>

/* 关/开全局中断 (mstatus.MIE, 对齐 core_riscv.h __disable_irq/__enable_irq 的 0x88 掩码) */
#define ATOMIC_IRQ_DISABLE() __asm volatile("csrc 0x800, %0" : : "r"(0x88))
#define ATOMIC_IRQ_ENABLE() __asm volatile("csrs 0x800, %0" : : "r"(0x88))

unsigned char __atomic_exchange_1(volatile void* ptr, unsigned char val, int memorder)
{
    (void)memorder;
    uint8_t* p = (uint8_t*)ptr;
    ATOMIC_IRQ_DISABLE();
    uint8_t old = *p;
    *p = val;
    ATOMIC_IRQ_ENABLE();
    return old;
}

_Bool __atomic_compare_exchange_1(volatile void* ptr, void* expected, unsigned char desired,
                                  _Bool weak, int success_memorder, int failure_memorder)
{
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    uint8_t* p = (uint8_t*)ptr;
    uint8_t exp = *(uint8_t*)expected;
    ATOMIC_IRQ_DISABLE();
    uint8_t old = *p;
    if (old == exp)
    {
        *p = desired;
        ATOMIC_IRQ_ENABLE();
        return 1;
    }
    ATOMIC_IRQ_ENABLE();
    *(uint8_t*)expected = old;
    return 0;
}
