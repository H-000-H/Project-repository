/*
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file redef.h
 * @brief Host test harness stub for redef.h
 *
 * The real inc/redef.h contains Cortex-M inline assembly (mrs primask, cpsid),
 * which cannot compile on the host. This stub replaces it when building the
 * kernel for host testing: same type/macro names (mini_os_/MINI_OS_),
 * host-friendly irq no-ops and __atomic builtins. The test build injects this
 * file first via "-include redef.h", and the REDEF_H guard then blocks the
 * real inc/redef.h.
 *
 * Covers the parts used by memory.c / err.h and, since the idle-thread test,
 * thread.c / schedule.c / mutex.c / semaphore.c: the tick/user-data typedefs,
 * bit helpers, function attributes, barrier/wfi no-ops and dummy MMIO lvalues
 * for the PendSV/SYSTICK registers (schedule_start/systick_init only need
 * them to compile; they are never executed on the host).
 */
#ifndef REDEF_H
#define REDEF_H

#include <stddef.h>

/* ------------------------------- base types ------------------------------ */
typedef signed char         mini_os_int8_t;
typedef signed short        mini_os_int16_t;
typedef signed int          mini_os_int32_t;
typedef signed long long    mini_os_int64_t;
typedef unsigned char       mini_os_uint8_t;
typedef unsigned short      mini_os_uint16_t;
typedef unsigned int        mini_os_uint32_t;
typedef unsigned long long  mini_os_uint64_t;

typedef mini_os_int8_t      mini_os_bool_t;
typedef size_t              mini_os_size_t;
typedef signed long         mini_os_tick_t;      /**< OS tick count */
typedef signed long         mini_os_user_data_t; /**< thread user data slot */

/* ----------------------------- self types -------------------------------- */
typedef signed long         mini_os_irq_t;
typedef mini_os_int32_t     mini_os_err_t;

#define MINI_OS_TRUE    1
#define MINI_OS_FALSE   0
#define MINI_OS_UINT8_MAX 0xFFU
#define MINI_OS_WAIT_FOREVER ((mini_os_tick_t)-1)
#define MINI_OS_NULL    ((void *)0)

#define MINI_OS_STATIC_INLINE static inline
#define MINI_OS_SEQ_CST __ATOMIC_SEQ_CST

/* --------------------- atomic types (host: plain + builtins) --------------- */
typedef mini_os_int8_t   mini_os_atomic_int8_t;
typedef mini_os_uint8_t  mini_os_atomic_uint8_t;
typedef mini_os_int32_t  mini_os_atomic_int32_t;
typedef mini_os_uint32_t mini_os_atomic_uint32_t;

/* --------------------- constructors (host: clang attribute) --------------- */
#define MINI_OS_CONSTRUCTOR(x) __attribute__((constructor(x)))

/* --------------------- compile-time alignment ----------------------------- */
#define MINI_OS_ALIGN(x) __attribute__((aligned(x)))

/* --------------------- static assert --------------------------------------- */
#define MINI_OS_ASSERT(condition, fmt) _Static_assert(condition, fmt)

/* --------------------------- container_of ---------------------------------- */
#define mini_os_container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))

/* ----------------------- string/memory builtins ---------------------------- */
#define MINI_OS_MEMSET(dst, val, n) __builtin_memset((dst), (val), (n))
#define MINI_OS_MEMCPY(dst, src, n) __builtin_memcpy((dst), (src), (n))
#define MINI_OS_STRCMP(s1, s2)      __builtin_strcmp((s1), (s2))

/* ------------------------ power-of-2 alignment ----------------------------- */
#define MINI_OS_MEMORY_ALIGN_UP(x, a) \
    ((((size_t)(x)) + ((size_t)(a) - 1U)) & (~((size_t)(a) - 1U)))
#define MINI_OS_MEMORY_ALIGN_DOWN(x, a) \
    (((size_t)(x)) & ~((size_t)(a) - 1U))

/* ------------------------ thread name copy helper -------------------------- */
MINI_OS_STATIC_INLINE void mini_os_set_name(char* dst, const char* name, mini_os_size_t size)
{
    mini_os_size_t i = 0u;
    if (name != MINI_OS_NULL)
    {
        while (name[i] != '\0' && i < size - 1u)
        {
            dst[i] = name[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ------------------------ bit helpers (host: builtins) --------------------- */
#define MINI_OS_CTZ(x)      __builtin_ctz(x)
#define MINI_OS_CLZ(x)      __builtin_clz(x)
#define MINI_OS_POPCOUNT(x) __builtin_popcount(x)

/* --------------------- function attributes (host) -------------------------- */
#define MINI_OS_NO_RETURN __attribute__((noreturn))
/* MinGW/COFF ld cannot resolve a weak definition that lives in a plain object
 * file (it reports the symbol as undefined even when the definition is on the
 * command line). Host tests never override a weak kernel hook, so the
 * attribute is dropped for that toolchain only; ELF hosts keep it. */
#if defined(__MINGW32__)
#define MINI_OS_WEAK
#else
#define MINI_OS_WEAK      __attribute__((weak))
#endif
#define MINI_OS_UNUSED(x) __attribute__((unused))

/* -------------------- host irq layer (interrupts = no-op) ----------------- */
MINI_OS_STATIC_INLINE mini_os_irq_t mini_os_irq_save(void)
{
    return 0;
}

MINI_OS_STATIC_INLINE void mini_os_irq_restore(mini_os_irq_t irq_level)
{
    (void)irq_level;
}

void mini_os_irq_enable(void);   /**< unmask interrupts: port function, host stub no-op */

/* ------------- host MMIO dummies (only need to compile, never run) -------- */
#define MINI_OS_PENDSV_IRQ            (*(volatile unsigned char *)0)
#define MINI_OS_SYSTICK_IRQ           (*(volatile unsigned char *)0)
#define MINI_OS_SYSTICK_CTRL          (*(volatile unsigned int *)0)
#define MINI_OS_SYSTICK_RELOAD        (*(volatile unsigned int *)0)
#define MINI_OS_SYSTICK_VAL           (*(volatile unsigned int *)0)
#define MINI_OS_SYSTICK_CTRL_ENABLE    (1u << 0)
#define MINI_OS_SYSTICK_CTRL_TICKINT   (1u << 1)
#define MINI_OS_SYSTICK_CTRL_CLKSOURCE (1u << 2)

/* ------------------------ host atomics (clang builtins) ------------------- */
#define MINI_OS_RELAXED __ATOMIC_RELAXED
#define MINI_OS_ACQUIRE __ATOMIC_ACQUIRE
#define MINI_OS_RELEASE __ATOMIC_RELEASE
#define MINI_OS_ACQ_REL __ATOMIC_ACQ_REL
#define MINI_OS_ATOMIC_STORE(ptr, value, mem) __atomic_store_n((ptr), (value), (mem))
#define MINI_OS_ATOMIC_LOAD(ptr, mem)         __atomic_load_n((ptr), (mem))
/* returns TRUE when the byte was ALREADY set (lock still held) */
#define MINI_OS_ATOMIC_TEST_AND_SET(ptr, mem) __atomic_test_and_set((ptr), (mem))
#define MINI_OS_ATOMIC_EXCHANGE(ptr, value, mem) __atomic_exchange_n((ptr), (value), (mem))
#define MINI_OS_ATOMIC_INIT(ptr, val)         MINI_OS_ATOMIC_STORE((ptr), (val), MINI_OS_RELAXED)
#define MINI_OS_ATOMIC_CAS(ptr, expected, desired, mem_success, mem_fail) \
    __atomic_compare_exchange_n((ptr), (expected), (desired), 0, (mem_success), (mem_fail))

#endif /* REDEF_H */
