/*
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file test_idle.c
 * @brief Host test for the idle thread (create / lowest priority / corpse reaping)
 *
 * Build & run (host, no ARM toolchain needed):
 *   clang -std=c11 -Wall -Wextra -Itest -Iinc -include redef.h test/test_idle.c
 *         src/thread.c src/schedule.c src/mutex.c src/semaphore.c src/memory.c
 *         -o build/test_idle.exe && build/test_idle.exe
 *
 * Add -DMINI_OS_THREAD_DETACH=1 to exercise the join retval check as well.
 *
 * -include redef.h lets test/redef.h (host stubs) shadow the real inc/redef.h
 * (Cortex-M inline asm). There is no second stack on the host, so the harness
 * simulates the scheduler cooperatively:
 *   - mini_os_yield_trigger() longjmps back to the harness = "switch away now";
 *   - a worker runs once on the harness stack and calls exit() (never returns);
 *   - one idle iteration is driven through the public mini_os_thread_idle_hook()
 *     with a hook that longjmps out right after the reaper ran.
 * The harness reads mini_os_current_thread to observe which thread the
 * scheduler picked, and mini_os_thread_get_idle_handle() to prove the pick is
 * the idle thread itself.
 */
#include "thread.h"
#include "schedule.h"
#include "memory.h"
#include "port.h"
#include "err.h"

#include <setjmp.h>
#include <stdio.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (cond) {                                                     \
            printf("ok   %-58s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
        } else {                                                        \
            printf("FAIL %-58s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
        fflush(stdout);                                                 \
    } while (0)

/* Linker heap symbol stubs: same pattern as test_mem.c (start is the buffer,
 * end right after it, MINI_OS_HEAP_SIZE = buffer size; the startup constructor
 * takes the buffer over before main) */
char __mini_os_heap_start[16384] __attribute__((aligned(8)));
char __mini_os_heap_end[1];

/* --------------------------- port stubs (host) ---------------------------- */
void mini_os_psp_set(mini_os_uint32_t psp)                 { (void)psp; }
void mini_os_set_control(mini_os_uint32_t control)         { (void)control; }
void mini_os_irq_enable(void)                              { }
void mini_os_barrier(void)                                 { }
void mini_os_wfi(void)                                     { }

/* ------------------------ cooperative scheduler --------------------------- */
extern mini_os_thread_t *mini_os_current_thread; /* the picked thread: port restores its SP */
extern mini_os_err_t mini_os_schedule_switch(void); /* the pick routine PendSV calls (no public header) */

static jmp_buf g_sched;      /* armed per round: yield inside a thread returns here */

/* PendSV stand-in: any yield/switch-away returns to the harness loop */
void mini_os_yield_trigger(void)
{
    longjmp(g_sched, 1);
}

/* ------------------------------- worker ----------------------------------- */
static void * volatile s_worker_retval = (void *)0xC0FFEEU;

static void worker_entry(void *param)
{
    (void)param;
    mini_os_thread_exit((void *)s_worker_retval); /* never returns: longjmps via the yield */
    CHECK(!"worker entry returned from exit");    /* unreached */
}

/* ----------------------------- idle round --------------------------------- */
static jmp_buf g_idle_round;         /* one idle iteration ends in this hook */
static volatile int s_idle_hook_ran = 0;

static void idle_break(void *param)
{
    (void)param;
    s_idle_hook_ran = 1;
    longjmp(g_idle_round, 1);        /* out of the idle loop: reaper already ran */
}

/* --------------------------------------------------------------------------- */
int main(void)
{
    mini_os_thread_t *worker;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    mini_os_uint8_t prio = 0;
    mini_os_size_t free_before;
    mini_os_size_t free_after;

    CHECK(mini_os_heap_free_space() > 0); /* heap constructor ran before main */

    CHECK(mini_os_schedule_init() == MINI_OS_OK);
    mini_os_thread_idle_create();         /* 256-byte default stack: must exist now */
    CHECK(mini_os_thread_get_idle_handle() != MINI_OS_NULL);

    worker = mini_os_thread_create("t_worker", 512, 10, worker_entry, MINI_OS_NULL);
    CHECK(worker != MINI_OS_NULL);

    /* round 1: worker (prio 10) beats the idle thread (prio MINI_OS_PRIORITY-1) */
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == worker);
        CHECK(mini_os_thread_get_state(worker, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_RUNNING);
        worker_entry(MINI_OS_NULL);       /* runs exit() -> longjmp back here */
        CHECK(!"worker entry returned to scheduler");
    }
    /* back from the yield inside exit(): the worker is a queued corpse now */
    CHECK(mini_os_thread_get_state(worker, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_TERMINATED);

    /* round 2: nothing else ready -> the idle thread must be picked */
    if (setjmp(g_idle_round) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == mini_os_thread_get_idle_handle());
        CHECK(mini_os_thread_get_state(mini_os_current_thread, &state) == MINI_OS_OK &&
              state == MINI_OS_THREAD_STATE_RUNNING);
        CHECK(mini_os_thread_get_priority(mini_os_current_thread, &prio) == MINI_OS_OK &&
              prio == MINI_OS_PRIORITY - 1); /* lowest level, never starves others */

#if MINI_OS_THREAD_DETACH
        {
            void *retval = MINI_OS_NULL;

            /* fast path: target already terminated, corpse still pinned for nobody.
             * Called from the idle context standing in for "another thread" - the
             * self-join guard must see current != worker here, and the corpse must
             * still be allocated (the reaper has not run yet). */
            CHECK(mini_os_thread_join(worker, &retval, 0) == MINI_OS_OK);
            CHECK(retval == (void *)s_worker_retval);
        }
#endif

        free_before = mini_os_heap_free_space();
        (void)mini_os_thread_idle_hook(idle_break, MINI_OS_NULL); /* reaper -> hook -> longjmp */
        CHECK(!"idle loop returned without the hook");
    }
    CHECK(s_idle_hook_ran == 1);
    free_after = mini_os_heap_free_space();
    CHECK(free_after > free_before);      /* idle reaped the corpse: TCB + stack back */

    /* round 3: with only the idle thread left it stays scheduled (self loop) */
    CHECK(mini_os_schedule_switch() == MINI_OS_OK);
    CHECK(mini_os_current_thread == mini_os_thread_get_idle_handle());
    CHECK(mini_os_thread_get_priority(mini_os_current_thread, &prio) == MINI_OS_OK &&
          prio == MINI_OS_PRIORITY - 1);
    CHECK(mini_os_thread_get_state(mini_os_current_thread, &state) == MINI_OS_OK &&
          state == MINI_OS_THREAD_STATE_RUNNING);

    printf("\n%d checks, %d failures\n", checks, failures);
    return (failures == 0) ? 0 : 1;
}
