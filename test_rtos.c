/*
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file test_rtos.c
 * @brief Host test for the WHOLE mini-os kernel with every feature switched on
 *
 * Build & run (host, no ARM toolchain needed):
 *   clang -std=c11 -Wall -Wextra -Itest -Iinc -include redef.h
 *         -DMINI_OS_THREAD_DETACH=1 -DMINI_OS_EVENT=1 -DMINI_OS_FIND_BY_NAME=1
 *         -DMINI_OS_TIME_SLICE=1
 *         test/test_rtos.c src/thread.c src/schedule.c src/mutex.c
 *         src/semaphore.c src/timer.c src/queue.c src/event.c src/memory.c
 *         -o build/test_rtos.exe && build/test_rtos.exe
 *
 * -include redef.h lets test/redef.h (host stubs) shadow the real inc/redef.h
 * (Cortex-M inline asm). There is no second stack on the host, so the harness
 * simulates the scheduler cooperatively:
 *   - mini_os_yield_trigger() longjmps back to the harness = "switch away now";
 *   - a thread runs once on the harness stack; if it blocks (parks) the C
 *     function cannot resume mid-call, so blocking/waking is verified through
 *     the observable state machine (BLOCKED -> wheel -> READY) instead;
 *   - ticks are simulated by calling mini_os_systick_handler() directly, which
 *     drives the thread time wheel AND the timer wheel (hard callbacks run
 *     right there, soft callbacks queue for the timer service thread);
 *   - the timer service thread is entered the same cooperative way: its loop
 *     is restartable, so one invocation = one take + one pending drain.
 */
#include "thread.h"
#include "schedule.h"
#include "semaphore.h"
#include "mutex.h"
#include "queue.h"
#include "event.h"
#include "timer.h"
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

/* Linker heap symbol stubs: same pattern as test_idle.c. Sized for the 80
 * worker fleet (TCB + minimum stack each) plus the timer module. */
char __mini_os_heap_start[131072] __attribute__((aligned(8)));
char __mini_os_heap_end[1];

/* --------------------------- port stubs (host) ---------------------------- */
void mini_os_psp_set(mini_os_uint32_t psp)                 { (void)psp; }
void mini_os_set_control(mini_os_uint32_t control)         { (void)control; }
void mini_os_irq_enable(void)                              { }
void mini_os_barrier(void)                                 { }
void mini_os_wfi(void)                                     { }

/* ------------------------ cooperative scheduler --------------------------- */
extern mini_os_thread_t *mini_os_current_thread;
extern mini_os_err_t mini_os_schedule_switch(void);

static jmp_buf g_sched;         /* armed per round: yield inside a thread returns here */

void mini_os_yield_trigger(void)
{
    longjmp(g_sched, 1);
}

/* ------------------------------ helpers ----------------------------------- */
static void reset_current(void)
{
    mini_os_current_thread = MINI_OS_NULL;
}

/* Simulate n SysTick interrupts: advances the tick, the thread wheel, the
 * timeslice and the timer wheel (hard callbacks fire right here) */
static void drive_ticks(int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        mini_os_systick_handler();
    }
}

static mini_os_tick_t now(void)
{
    mini_os_tick_t tick = 0;

    (void)mini_os_get_tick(&tick);
    return tick;
}

static void drive_until(mini_os_tick_t target)
{
    while (now() < target)
    {
        drive_ticks(1);
    }
}

/* Pick the highest-priority ready thread (must be `t`) and run its entry once
 * on the harness stack; any yield inside returns via the longjmp above */
static void run_thread_once(mini_os_thread_t *t)
{
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == t);
        t->entry(t->param);
    }
}

/* Run one iteration of the timer service thread: it takes the semaphore,
 * drains every queued soft callback, re-arms periodic timers and parks again */
static void run_timer_thread(void)
{
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread != MINI_OS_NULL);
        CHECK(mini_os_current_thread->priority == MINI_OS_TIMER_THREAD_PRIORITY);
        mini_os_current_thread->entry(mini_os_current_thread->param);
    }
}

/* Delete a thread that is no longer needed (works from any state except
 * "is the current thread", which reset_current() clears first) */
static void finish_thread(mini_os_thread_t *t)
{
    reset_current();
    CHECK(mini_os_thread_delete(t) == MINI_OS_OK);
}

/* ===================== 1. thread lifecycle & boundaries ==================== */
static void noop_entry(void *param)
{
    (void)param;
}

static volatile int s_flag = 0;

static void self_delete_entry(void *param)
{
    (void)param;
    s_flag = (mini_os_thread_delete(mini_os_thread_current()) == MINI_OS_ERR_INVAL);
}

static void test_thread_basics(void)
{
    mini_os_thread_t *t;
    static mini_os_thread_t tcb;
    static MINI_OS_ALIGN(8) mini_os_uint32_t stk[128];
    static MINI_OS_ALIGN(8) char abuf[300];
    static char longname[64];
    char name[MINI_OS_THREADS_NAME_LEN + 8];
    mini_os_uint32_t name_len = 0;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    mini_os_uint8_t prio = 0;
    mini_os_user_data_t ud = 0;
    mini_os_size_t i;

    printf("--- thread lifecycle & boundaries ---\n");

    /* dynamic create: every invalid argument rejected */
    CHECK(mini_os_thread_create(MINI_OS_NULL, 256, 5, noop_entry, MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_thread_create("t", 0, 5, noop_entry, MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_thread_create("t", 256, 5, MINI_OS_NULL, MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_thread_create("t", MINI_OS_THREAD_MIN_STACK_SIZE - 8, 5, noop_entry, MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_thread_create("t", 260, 5, noop_entry, MINI_OS_NULL) == MINI_OS_NULL); /* not 8-aligned */
    CHECK(mini_os_thread_create("t", 256, MINI_OS_PRIORITY, noop_entry, MINI_OS_NULL) == MINI_OS_NULL);

    /* minimum stack size and one priority step below the limit are legal */
    t = mini_os_thread_create("dyn", MINI_OS_THREAD_MIN_STACK_SIZE, MINI_OS_PRIORITY - 1,
                              noop_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    finish_thread(t);

    /* static create: NULL buffers rejected, an unaligned stack buffer rejected */
    CHECK(mini_os_thread_create_static("ts", 256, 5, noop_entry, MINI_OS_NULL, MINI_OS_NULL, &tcb) == MINI_OS_NULL);
    CHECK(mini_os_thread_create_static("ts", 256, 5, noop_entry, MINI_OS_NULL, stk, MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_thread_create_static("ts", 256, 5, noop_entry, MINI_OS_NULL,
                                       (mini_os_uint32_t *)(abuf + 4), &tcb) == MINI_OS_NULL);

    t = mini_os_thread_create_static("ts", 512, 5, noop_entry, MINI_OS_NULL, stk, &tcb);
    CHECK(t != MINI_OS_NULL);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);

    /* name roundtrip + truncation boundary: only NAME_LEN-1 characters kept */
    for (i = 0; i < sizeof(longname) - 1u; i++)
    {
        longname[i] = (char)('A' + (i % 26));
    }
    longname[sizeof(longname) - 1u] = '\0';
    CHECK(mini_os_thread_set_name(t, longname) == MINI_OS_OK);
    CHECK(mini_os_thread_get_name(t, name, &name_len) == MINI_OS_OK);
    CHECK(name_len == MINI_OS_THREADS_NAME_LEN - 1);
    CHECK(name[MINI_OS_THREADS_NAME_LEN - 1] == '\0');
    CHECK(mini_os_thread_set_name(MINI_OS_NULL, "x") == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_name(MINI_OS_NULL, name, &name_len) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_name(t, MINI_OS_NULL, &name_len) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_name(t, name, MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    /* priority boundaries: the lowest level is legal, MINI_OS_PRIORITY is not */
    CHECK(mini_os_thread_get_priority(t, &prio) == MINI_OS_OK && prio == 5);
    CHECK(mini_os_thread_set_priority(t, MINI_OS_PRIORITY) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_set_priority(t, MINI_OS_PRIORITY - 1) == MINI_OS_OK);
    CHECK(mini_os_thread_get_priority(t, &prio) == MINI_OS_OK && prio == MINI_OS_PRIORITY - 1);
    CHECK(mini_os_thread_set_priority(t, 5) == MINI_OS_OK);
    CHECK(mini_os_thread_get_priority(MINI_OS_NULL, &prio) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_priority(t, MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    /* state / user data boundaries (negative user data is a legal slot value) */
    CHECK(mini_os_thread_get_state(MINI_OS_NULL, &state) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_state(t, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_set_user_data(t, -12345) == MINI_OS_OK);
    CHECK(mini_os_thread_get_user_data(t, &ud) == MINI_OS_OK && ud == -12345);
    CHECK(mini_os_thread_set_user_data(MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_get_user_data(MINI_OS_NULL, &ud) == MINI_OS_ERR_INVAL);

#if MINI_OS_TIME_SLICE
    /* timeslice roundtrip (the thread is never current while the slice is armed) */
    CHECK(mini_os_thread_set_timeslice(t, 7) == MINI_OS_OK);
    {
        mini_os_tick_t slice = 0;

        CHECK(mini_os_thread_get_timeslice(t, &slice) == MINI_OS_OK && slice == 7);
    }
    CHECK(mini_os_thread_set_timeslice(MINI_OS_NULL, 1) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_set_timeslice(t, -1) == MINI_OS_ERR_INVAL);
#endif

    /* suspend / resume state machine, double suspend is idempotent */
    CHECK(mini_os_thread_suspend(t) == MINI_OS_OK);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_SUSPENDED);
    CHECK(mini_os_thread_suspend(t) == MINI_OS_OK);
    CHECK(mini_os_thread_resume(t) == MINI_OS_OK);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
    CHECK(mini_os_thread_resume(t) == MINI_OS_ERR_INVAL); /* not suspended any more */
    CHECK(mini_os_thread_suspend(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_resume(MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    /* delay boundaries outside thread context */
    CHECK(mini_os_thread_delay_tick(0) == MINI_OS_OK);
    CHECK(mini_os_thread_delay_tick_until(0) == MINI_OS_OK);
    CHECK(mini_os_thread_delay_tick_until(100) == MINI_OS_ERR_INVAL); /* needs a thread context */
    CHECK(mini_os_thread_delay_ms(0) == MINI_OS_OK);

    /* delete boundaries */
    CHECK(mini_os_thread_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_delete_static(MINI_OS_NULL, stk, &tcb) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_delete_static(t, MINI_OS_NULL, &tcb) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_delete_static(t, stk, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_thread_delete_static(t, stk, &tcb) == MINI_OS_OK);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_INIT);

    /* deleting the current thread from inside is refused */
    t = mini_os_thread_create("selfdel", 512, 6, self_delete_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    s_flag = 0;
    run_thread_once(t);
    CHECK(s_flag == 1);
    finish_thread(t);
}

/* ===================== 2. scheduler pick / round-robin ===================== */
static void test_schedule(void)
{
    mini_os_thread_t *a;
    mini_os_thread_t *b;

    printf("--- scheduler: pick + same-priority round-robin ---\n");
    a = mini_os_thread_create("sched_a", 512, 5, noop_entry, MINI_OS_NULL);
    b = mini_os_thread_create("sched_b", 512, 5, noop_entry, MINI_OS_NULL);
    CHECK(a != MINI_OS_NULL && b != MINI_OS_NULL);

    /* highest priority first: the head of the prio-5 list */
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == a);
    }

    /* round-robin: a already ran (still runnable), the successor is picked */
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == b);
    }

    /* a priority change re-links a READY thread: prio 2 beats prio 5 */
    CHECK(mini_os_thread_set_priority(a, 2) == MINI_OS_OK);
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == a);
    }
    CHECK(mini_os_thread_set_priority(a, 5) == MINI_OS_OK);

    finish_thread(a);
    finish_thread(b);

    /* nothing ready left (no idle thread yet): the switch reports NODEV */
    CHECK(mini_os_schedule_switch() == MINI_OS_ERR_NODEV);
}

/* =========================== 3. semaphore ================================== */
static void test_semaphore(void)
{
    mini_os_semaphore_t *s;
    mini_os_uint16_t count = 0;
    static mini_os_semaphore_t ss;

    printf("--- semaphore: counting, saturation, conversions ---\n");
    CHECK(mini_os_semaphore_create("x", 0, 0) == MINI_OS_NULL);   /* max 0 */
    CHECK(mini_os_semaphore_create("x", 2, 3) == MINI_OS_NULL);   /* count > max */
    CHECK(mini_os_semaphore_create_static(MINI_OS_NULL, 2, 1, MINI_OS_NULL) == MINI_OS_NULL);

    s = mini_os_semaphore_create("cnt", 3, 0);
    CHECK(s != MINI_OS_NULL);
    CHECK(mini_os_semaphore_get_count(MINI_OS_NULL, &count) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_get_count(s, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_take(MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_take(s, 0) == MINI_OS_ERR_AGAIN);     /* empty, non-blocking */
    CHECK(mini_os_semaphore_try_take(s) == MINI_OS_ERR_AGAIN);
    CHECK(mini_os_semaphore_give(MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    /* fill to saturation: the 4th give must fail */
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_ERR_BUSY);
    CHECK(mini_os_semaphore_give_isr(s) == MINI_OS_ERR_BUSY);     /* ISR give saturated */
    CHECK(mini_os_semaphore_get_count(s, &count) == MINI_OS_OK && count == 3);

    /* drain: the take past zero fails again */
    CHECK(mini_os_semaphore_take(s, 0) == MINI_OS_OK);
    CHECK(mini_os_semaphore_take(s, 0) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give_isr(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_take(s, 0) == MINI_OS_OK);
    CHECK(mini_os_semaphore_take(s, 0) == MINI_OS_OK);
    CHECK(mini_os_semaphore_try_take(s) == MINI_OS_ERR_AGAIN);

    /* binary conversion boundary: only a saturated semaphore collapses */
    CHECK(mini_os_semaphore_to_binary(s) == MINI_OS_ERR_BUSY);    /* count 0 != max 3 */
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);               /* saturated again */
    CHECK(mini_os_semaphore_to_binary(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_get_count(s, &count) == MINI_OS_OK && count == 1);
    CHECK(mini_os_semaphore_to_binary(s) == MINI_OS_OK);          /* already binary: no-op */
    CHECK(mini_os_semaphore_to_counting(MINI_OS_NULL, 5) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_to_counting(s, 1) == MINI_OS_ERR_INVAL);  /* max < 2 */
    CHECK(mini_os_semaphore_to_counting(s, 5) == MINI_OS_OK);
    CHECK(mini_os_semaphore_get_count(s, &count) == MINI_OS_OK && count == 1);
    CHECK(mini_os_semaphore_to_counting(s, 5) == MINI_OS_ERR_NOTSUPP); /* no longer binary */
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_OK);
    CHECK(mini_os_semaphore_give(s) == MINI_OS_ERR_BUSY);         /* widened max 5 saturated */

    /* delete variants */
    CHECK(mini_os_semaphore_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_delete_static(s) == MINI_OS_ERR_NOTSUPP); /* heap-owned */
    CHECK(mini_os_semaphore_delete(s) == MINI_OS_OK);

    CHECK(mini_os_semaphore_create_static("st", 2, 1, &ss) != MINI_OS_NULL);
    CHECK(mini_os_semaphore_delete(&ss) == MINI_OS_ERR_NOTSUPP);  /* caller-owned storage */
    CHECK(mini_os_semaphore_delete_static(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_semaphore_delete_static(&ss) == MINI_OS_OK);

#if MINI_OS_FIND_BY_NAME
    CHECK(mini_os_get_semaphore_by_name("st") == MINI_OS_NULL);   /* deleted */
    s = mini_os_semaphore_create("named_s", 1, 1);
    CHECK(s != MINI_OS_NULL && mini_os_get_semaphore_by_name("named_s") == s);
    CHECK(mini_os_get_semaphore_by_name(MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_semaphore_delete(s) == MINI_OS_OK);
#endif
}

/* ============================= 4. mutex ==================================== */
static mini_os_mutex_t *s_mx = MINI_OS_NULL;
static volatile int s_phase = 0;

static void mx_recursive_entry(void *param)
{
    int i;

    (void)param;
    s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_OK);
    for (i = 0; (i < 254) && s_flag; i++)       /* deepen to depth 255 */
    {
        s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_OK);
    }
    if (s_flag)
    {
        s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_ERR_BUSY); /* depth overflow */
    }
    for (i = 0; (i < 255) && s_flag; i++)       /* unwind every level */
    {
        s_flag = (mini_os_mutex_unlock(s_mx) == MINI_OS_OK);
    }
    if (s_flag)
    {
        s_flag = (mini_os_mutex_unlock(s_mx) == MINI_OS_ERR_INVAL); /* over-unlock */
    }
}

static void mx_owner_entry(void *param)
{
    (void)param;
    if (s_phase == 0)
    {
        s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_OK);       /* acquire, stay owner */
    }
    else if (s_phase == 1)
    {
        s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_ERR_BUSY); /* non-recursive re-lock */
    }
    else if (s_phase == 2)
    {
        s_flag = (mini_os_mutex_unlock(s_mx) == MINI_OS_OK);        /* release */
    }
    else
    {
        s_flag = (mini_os_mutex_unlock(s_mx) == MINI_OS_ERR_INVAL); /* after kill-delete */
    }
}

static void mx_contender_entry(void *param)
{
    (void)param;
    s_flag = (mini_os_mutex_lock(s_mx, 0) == MINI_OS_ERR_AGAIN);    /* contested, no block */
}

static void mx_pi_entry(void *param)
{
    (void)param;
    (void)mini_os_mutex_lock(s_mx, 5);   /* boosts the owner, parks, longjmps */
    CHECK(!"PI contender resumed mid-call (impossible on the host harness)");
}

static void mx_wrong_owner_entry(void *param)
{
    (void)param;
    s_flag = (mini_os_mutex_unlock(s_mx) == MINI_OS_ERR_INVAL);     /* never owned it */
}

/* how many threads are currently parked on a mutex' wait queue */
static int mx_waiter_count(mini_os_mutex_t *m)
{
    mini_os_list_t *node;
    int n = 0;

    for (node = m->semaphore.wait_list.next; node != &m->semaphore.wait_list; node = node->next)
    {
        n++;
    }
    return n;
}

/* set when a call that woke a thread reached its caller instead of yielding */
static volatile int s_no_yield = 0;

static void test_mutex(void)
{
    mini_os_thread_t *owner;
    mini_os_thread_t *other;
    mini_os_mutex_t *m;
    mini_os_uint8_t prio = 0;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    static mini_os_mutex_t mk;

    printf("--- mutex: recursion, contention, priority inheritance ---\n");

    /* no thread context yet: ownership needs a current thread */
    reset_current();
    CHECK(mini_os_mutex_lock(MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_mutex_unlock(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_mutex_create_static(MINI_OS_NULL, "m") == MINI_OS_NULL);
    CHECK(mini_os_mutex_enable_kill(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    {
        static mini_os_mutex_t tmp;

        CHECK(mini_os_mutex_create_static(&tmp, "tmpm") != MINI_OS_NULL);
        CHECK(mini_os_mutex_lock(&tmp, 0) == MINI_OS_ERR_INVAL);    /* no thread context */
        CHECK(mini_os_mutex_unlock(&tmp) == MINI_OS_ERR_INVAL);     /* no owner */
        CHECK(mini_os_mutex_delete_static(&tmp) == MINI_OS_OK);
    }

    /* recursive mutex: deepen to the UINT8_MAX boundary, overflow, unwind */
    m = mini_os_mutex_recuring_create("rec");
    CHECK(m != MINI_OS_NULL);
    s_mx = m;
    owner = mini_os_thread_create("mx_rec", 512, 8, mx_recursive_entry, MINI_OS_NULL);
    CHECK(owner != MINI_OS_NULL);
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);
    finish_thread(owner);
    CHECK(mini_os_mutex_delete(m) == MINI_OS_OK);                   /* free again */

    /* non-recursive mutex: owner, re-lock BUSY, contested AGAIN, PI */
    m = mini_os_mutex_create("plain");
    CHECK(m != MINI_OS_NULL);
    s_mx = m;
    s_phase = 0;
    owner = mini_os_thread_create("mx_owner", 512, 8, mx_owner_entry, MINI_OS_NULL);
    CHECK(owner != MINI_OS_NULL);
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);                                             /* owner holds it */
    s_phase = 1;
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);                                             /* re-lock refused */

    other = mini_os_thread_create("mx_try", 512, 7, mx_contender_entry, MINI_OS_NULL);
    CHECK(other != MINI_OS_NULL);
    s_flag = 0;
    run_thread_once(other);
    CHECK(s_flag == 1);                                             /* AGAIN, no park */
    finish_thread(other);

    /* priority inheritance: a higher-priority contender boosts the owner */
    other = mini_os_thread_create("mx_pi", 512, 3, mx_pi_entry, MINI_OS_NULL);
    CHECK(other != MINI_OS_NULL);
    run_thread_once(other);                                         /* parks (timeout 5) */
    CHECK(mini_os_thread_get_priority(owner, &prio) == MINI_OS_OK && prio == 3);
    CHECK(mini_os_thread_get_state(other, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
    drive_ticks(4);
    CHECK(mini_os_thread_get_state(other, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
    drive_ticks(1);                                                 /* timeout expiry */
    CHECK(mini_os_thread_get_state(other, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
    finish_thread(other);                                           /* the boost is still on */

    /* the owner releases: the boost drops, the base priority returns */
    s_phase = 2;
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);
    CHECK(mini_os_thread_get_priority(owner, &prio) == MINI_OS_OK && prio == 8);

    /* unlock by a thread that never owned it */
    other = mini_os_thread_create("mx_wrong", 512, 6, mx_wrong_owner_entry, MINI_OS_NULL);
    CHECK(other != MINI_OS_NULL);
    s_flag = 0;
    run_thread_once(other);
    CHECK(s_flag == 1);
    finish_thread(other);

    /* deleting a held mutex is refused until the owner lets go */
    s_phase = 0;
    s_flag = 0;
    run_thread_once(owner);                                         /* lock again */
    CHECK(s_flag == 1);
    CHECK(mini_os_mutex_delete(m) == MINI_OS_ERR_BUSY);
    s_phase = 2;
    s_flag = 0;
    run_thread_once(owner);                                         /* unlock */
    CHECK(s_flag == 1);
    finish_thread(owner);
    CHECK(mini_os_mutex_delete(m) == MINI_OS_OK);

    /* kill path: a static mutex can be force-released while held */
    CHECK(mini_os_mutex_create_static(&mk, "killm") != MINI_OS_NULL);
    s_mx = &mk;
    owner = mini_os_thread_create("mx_kill", 512, 8, mx_owner_entry, MINI_OS_NULL);
    CHECK(owner != MINI_OS_NULL);
    s_phase = 0;
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);                                             /* owner holds it */
    CHECK(mini_os_mutex_delete_static(&mk) == MINI_OS_ERR_BUSY);    /* kill not armed */
    CHECK(mini_os_mutex_enable_kill(&mk) == MINI_OS_OK);
    CHECK(mini_os_mutex_delete_static(&mk) == MINI_OS_OK);          /* force release */
    s_phase = 3;
    s_flag = 0;
    run_thread_once(owner);
    CHECK(s_flag == 1);                                             /* unlock now INVAL */
    finish_thread(owner);

    /* a holder dying while others are parked: every waiter must be failed and
     * the single unit handed back exactly once (kill_held -> kill_waiters),
     * with no stale wait_mutex / wait_node left behind */
    {
        static mini_os_mutex_t kw;
        mini_os_thread_t *w1;
        mini_os_thread_t *w2;
        mini_os_thread_t *taker;

        CHECK(mini_os_mutex_create_static(&kw, "killw") != MINI_OS_NULL);
        s_mx = &kw;
        s_phase = 0;
        s_flag = 0;
        owner = mini_os_thread_create("mx_khold", 512, 8, mx_owner_entry, MINI_OS_NULL);
        CHECK(owner != MINI_OS_NULL);
        run_thread_once(owner);
        CHECK(s_flag == 1);                                         /* prio 8 holds it */

        /* each waiter must be created and run before the next one exists, or
         * the scheduler picks the highest-priority newcomer instead */
        w1 = mini_os_thread_create("mx_kw1", 512, 5, mx_pi_entry, MINI_OS_NULL);
        CHECK(w1 != MINI_OS_NULL);
        reset_current();
        run_thread_once(w1);                                        /* parks, boosts to 5 */
        CHECK(mx_waiter_count(&kw) == 1);
        CHECK(mini_os_thread_get_priority(owner, &prio) == MINI_OS_OK && prio == 5);

        w2 = mini_os_thread_create("mx_kw2", 512, 4, mx_pi_entry, MINI_OS_NULL);
        CHECK(w2 != MINI_OS_NULL);
        reset_current();
        run_thread_once(w2);                                        /* parks behind w1 */
        CHECK(mx_waiter_count(&kw) == 2);
        CHECK(kw.owner == owner && kw.depth == 1u);
        CHECK(w1->wait_mutex == &kw && w2->wait_mutex == &kw);
        CHECK(mini_os_thread_get_state(w1, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
        CHECK(mini_os_thread_get_state(w2, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
        CHECK(mini_os_thread_get_priority(owner, &prio) == MINI_OS_OK && prio == 4);

        reset_current();
        /* freeing the holder wakes both waiters, so the delete path hands over
         * the CPU on its way out and never returns here (the yield is a
         * longjmp on the host): s_no_yield staying 0 IS the assertion */
        s_no_yield = 0;
        if (setjmp(g_sched) == 0)
        {
            (void)mini_os_thread_delete(owner);
            s_no_yield = 1;                                         /* only if it returned */
        }
        CHECK(s_no_yield == 0);
        CHECK(mx_waiter_count(&kw) == 0);                           /* queue drained */
        CHECK(kw.owner == MINI_OS_NULL && kw.depth == 0u);
        CHECK(kw.semaphore.count == kw.semaphore.max_count);        /* the unit is back */
        CHECK(w1->wait_mutex == MINI_OS_NULL && w2->wait_mutex == MINI_OS_NULL);
        CHECK(w1->wait_list == MINI_OS_NULL && w2->wait_list == MINI_OS_NULL);
        CHECK(mini_os_thread_get_state(w1, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
        CHECK(mini_os_thread_get_state(w2, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);

        /* exactly one taker walks away with the released unit */
        taker = mini_os_thread_create("mx_ktake", 512, 2, mx_owner_entry, MINI_OS_NULL);
        CHECK(taker != MINI_OS_NULL);
        s_phase = 0;
        s_flag = 0;
        reset_current();
        run_thread_once(taker);
        CHECK(s_flag == 1);
        CHECK(mini_os_mutex_delete_static(&kw) == MINI_OS_ERR_BUSY); /* held again */
        s_phase = 2;
        s_flag = 0;
        reset_current();
        run_thread_once(taker);
        CHECK(s_flag == 1);
        CHECK(mini_os_mutex_delete_static(&kw) == MINI_OS_OK);
        finish_thread(taker);
        finish_thread(w1);
        finish_thread(w2);
    }
}

/* ============================== 5. queue =================================== */
static void test_queue(void)
{
    mini_os_queue_t *q;
    int v = 0;
    int r = 0;
    static mini_os_queue_t qs;
    static unsigned char qbuf[255];                                 /* depth 255 x msg 1 */

    printf("--- queue: FIFO, wraparound, full/empty boundaries ---\n");
    CHECK(mini_os_queue_create("q", 0, 4) == MINI_OS_NULL);         /* msg_size 0 */
    CHECK(mini_os_queue_create("q", 4, 0) == MINI_OS_NULL);         /* depth 0 */
    CHECK(mini_os_queue_create_static("q", 4, 4, MINI_OS_NULL, qbuf, sizeof(qbuf)) == MINI_OS_NULL);
    CHECK(mini_os_queue_send(MINI_OS_NULL, &v, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_queue_receive(MINI_OS_NULL, &r, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_queue_send_isr(MINI_OS_NULL, &v) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_queue_receive_isr(MINI_OS_NULL, &r) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_queue_is_empty(MINI_OS_NULL) == MINI_OS_FALSE);   /* NULL is not a queue */
    CHECK(mini_os_queue_is_full(MINI_OS_NULL) == MINI_OS_FALSE);
    CHECK(mini_os_queue_get_depth(MINI_OS_NULL) == 0);

    q = mini_os_queue_create("q", (int)sizeof(int), 3);
    CHECK(q != MINI_OS_NULL);
    CHECK(mini_os_queue_is_empty(q) == MINI_OS_TRUE);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_ERR_AGAIN);    /* empty boundary */
    CHECK(mini_os_queue_receive_isr(q, &r) == MINI_OS_ERR_AGAIN);
    CHECK(mini_os_queue_send(q, MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_queue_receive(q, MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);

    /* fill to the full boundary, then overflow */
    for (v = 0; v < 3; v++)
    {
        CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    }
    CHECK(mini_os_queue_is_full(q) == MINI_OS_TRUE);
    CHECK(mini_os_queue_get_depth(q) == 3);
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_ERR_AGAIN);       /* full boundary */
    CHECK(mini_os_queue_send_isr(q, &v) == MINI_OS_ERR_AGAIN);

    /* FIFO order + both indices wrap around */
    for (v = 0; v < 3; v++)
    {
        CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == v);
    }
    CHECK(mini_os_queue_is_empty(q) == MINI_OS_TRUE);
    v = 10;
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    v = 11;
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    v = 12;
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == 10);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == 11);
    v = 13;                                                         /* write_idx wraps here */
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    v = 14;
    CHECK(mini_os_queue_send(q, &v, 0) == MINI_OS_OK);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == 12);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == 13);
    CHECK(mini_os_queue_receive(q, &r, 0) == MINI_OS_OK && r == 14);
    CHECK(mini_os_queue_get_depth(q) == 0);
    CHECK(mini_os_queue_delete(q) == MINI_OS_OK);

    /* uint8 depth boundary: 255 one-byte messages */
    q = mini_os_queue_create_static("q255", 1, 255, &qs, qbuf, sizeof(qbuf));
    CHECK(q != MINI_OS_NULL);
    {
        int i;
        unsigned char c = 0;
        unsigned char d = 0;

        for (i = 0; i < 255; i++)
        {
            c = (unsigned char)i;
            CHECK(mini_os_queue_send(q, &c, 0) == MINI_OS_OK);
        }
        CHECK(mini_os_queue_is_full(q) == MINI_OS_TRUE);
        CHECK(mini_os_queue_send(q, &c, 0) == MINI_OS_ERR_AGAIN);
        for (i = 0; i < 255; i++)
        {
            CHECK(mini_os_queue_receive(q, &d, 0) == MINI_OS_OK && d == (unsigned char)i);
        }
        CHECK(mini_os_queue_is_empty(q) == MINI_OS_TRUE);
    }
    CHECK(mini_os_queue_delete(q) == MINI_OS_ERR_NOTSUPP);          /* static storage */

    /* static pool too small */
    CHECK(mini_os_queue_create_static("small", 4, 4, &qs, qbuf, 8) == MINI_OS_NULL);
    CHECK(mini_os_queue_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
}

/* =========================== 6. event group ================================ */
#if MINI_OS_EVENT
static void test_event(void)
{
    mini_os_event_group_t *g;
    mini_os_uint32_t bits = 0;
    static mini_os_event_group_t eg;
    static mini_os_event_group_t egw;

    printf("--- event group: OR/WHOLE semantics, auto-clear ---\n");
    CHECK(mini_os_event_wait(MINI_OS_NULL, 1, 0, &bits) == MINI_OS_ERR_INVAL);
    g = mini_os_event_group_create_static(&eg, 0, MINI_OS_EVENT_OR_TYPE);
    CHECK(g != MINI_OS_NULL);
    CHECK(mini_os_event_get_group(g, &bits) == MINI_OS_OK && bits == 0);
    CHECK(mini_os_event_wait(g, 0, 0, &bits) == MINI_OS_ERR_INVAL); /* mask 0 */
    CHECK(mini_os_event_wait(g, 0x1, 0, &bits) == MINI_OS_ERR_AGAIN);
    CHECK(mini_os_event_set_group(MINI_OS_NULL, 1) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_event_clear_group(MINI_OS_NULL, 1) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_event_get_group(MINI_OS_NULL, &bits) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_event_group_set_auto_clear(MINI_OS_NULL, MINI_OS_FALSE) == MINI_OS_ERR_INVAL);

    /* auto-clear (default): a satisfied wait consumes its bits */
    CHECK(mini_os_event_set_group(g, 0x1) == MINI_OS_OK);
    CHECK(mini_os_event_wait(g, 0x1, 0, &bits) == MINI_OS_OK && bits == 0x1);
    CHECK(mini_os_event_get_group(g, &bits) == MINI_OS_OK && bits == 0);

    /* manual clear mode: the bits survive the wait */
    CHECK(mini_os_event_set_group(g, 0x3) == MINI_OS_OK);
    CHECK(mini_os_event_group_set_auto_clear(g, MINI_OS_FALSE) == MINI_OS_OK);
    CHECK(mini_os_event_wait(g, 0x2, 0, &bits) == MINI_OS_OK);      /* OR: any bit */
    CHECK(mini_os_event_get_group(g, &bits) == MINI_OS_OK && bits == 0x3);
    CHECK(mini_os_event_clear_group(g, 0x3) == MINI_OS_OK);
    CHECK(mini_os_event_get_group(g, &bits) == MINI_OS_OK && bits == 0);
    CHECK(mini_os_event_set_group_isr(g, 0x1) == MINI_OS_OK);       /* ISR variant */
    CHECK(mini_os_event_get_group(g, &bits) == MINI_OS_OK && bits == 0x1);
    CHECK(mini_os_event_group_delete(g) == MINI_OS_ERR_NOTSUPP);    /* static storage */

    /* WHOLE type: every bit of the mask must be set */
    g = mini_os_event_group_create_static(&egw, 0, MINI_OS_EVENT_WHOLE_TYPE);
    CHECK(g != MINI_OS_NULL);
    CHECK(mini_os_event_set_group(g, 0x1) == MINI_OS_OK);
    CHECK(mini_os_event_wait(g, 0x3, 0, &bits) == MINI_OS_ERR_AGAIN); /* 0x2 missing */
    CHECK(mini_os_event_set_group(g, 0x2) == MINI_OS_OK);           /* WHOLE type REPLACES */
    CHECK(mini_os_event_wait(g, 0x3, 0, &bits) == MINI_OS_ERR_AGAIN); /* 0x1 was replaced away */
    CHECK(mini_os_event_set_group(g, 0x3) == MINI_OS_OK);
    CHECK(mini_os_event_wait(g, 0x3, 0, &bits) == MINI_OS_OK && bits == 0x3);
    CHECK(mini_os_event_group_delete(g) == MINI_OS_ERR_NOTSUPP);

    /* heap variant: create + delete roundtrip */
    g = mini_os_event_group_create(0x5, MINI_OS_EVENT_OR_TYPE);
    CHECK(g != MINI_OS_NULL);
    CHECK(mini_os_event_group_delete(g) == MINI_OS_OK);
    CHECK(mini_os_event_group_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
}
#endif /* MINI_OS_EVENT */

/* ====================== 7. hard timer (tick context) ======================= */
static volatile int s_hard_count = 0;

static void hard_cb(void *param)
{
    (void)param;
    s_hard_count++;
}

static void hard_cb2(void *param)
{
    (void)param;
    s_hard_count += 100;
}

static void test_timer_hard(void)
{
    mini_os_timer_t *t;
    static mini_os_timer_t ts;
    mini_os_tick_t t0;

    printf("--- hard timer: wheel rounds, ISR-context callbacks ---\n");

    /* creation validation boundaries (NULL name is legal: unnamed timer) */
    t = mini_os_timer_create(MINI_OS_NULL, hard_cb, MINI_OS_NULL, 5, 0, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);
    CHECK(mini_os_timer_create("t", MINI_OS_NULL, MINI_OS_NULL, 5, 0, 0) == MINI_OS_NULL);
    CHECK(mini_os_timer_create("t", hard_cb, MINI_OS_NULL, 0, 0, 0) == MINI_OS_NULL);
    CHECK(mini_os_timer_create("t", hard_cb, MINI_OS_NULL, -1, 0, 0) == MINI_OS_NULL);
    CHECK(mini_os_timer_create("t", hard_cb, MINI_OS_NULL, 5, 2, 0) == MINI_OS_NULL);
    CHECK(mini_os_timer_create("t", hard_cb, MINI_OS_NULL, 5, 0, 1) == MINI_OS_NULL);
    CHECK(mini_os_timer_create("t", hard_cb, MINI_OS_NULL, 5, 0, 4) == MINI_OS_NULL);
    CHECK(mini_os_timer_create_static("t", hard_cb, MINI_OS_NULL, 5, 0, 0, MINI_OS_NULL) == MINI_OS_NULL);

    /* API NULL boundaries */
    CHECK(mini_os_timer_start(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_stop(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_delete_static(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_set_trigger_tick(MINI_OS_NULL, 5) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_set_callback(MINI_OS_NULL, hard_cb, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_set_callback(MINI_OS_NULL, MINI_OS_NULL, MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    /* one-shot fires exactly once at the requested tick */
    t = mini_os_timer_create("hard_1s", hard_cb, MINI_OS_NULL, 5, 0, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + 4);
    CHECK(s_hard_count == 0);
    drive_ticks(1);
    CHECK(s_hard_count == 1);
    CHECK((t->flag & MINI_OS_TIMER_FLAG_ACTIVE) == 0);              /* disarmed after firing */
    drive_until(t0 + 40);
    CHECK(s_hard_count == 1);                                       /* never fires again */

    /* restart uses the swapped callback */
    CHECK(mini_os_timer_set_callback(t, hard_cb2, MINI_OS_NULL) == MINI_OS_OK);
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + 5);
    CHECK(s_hard_count == 101);                                     /* 1 + 100 from the new cb */
    CHECK(mini_os_timer_stop(t) == MINI_OS_OK);                     /* cancels the fire */
    drive_until(t0 + 20);
    CHECK(s_hard_count == 101);
    CHECK(mini_os_timer_stop(t) == MINI_OS_OK);                     /* double stop: harmless */
    CHECK(mini_os_timer_set_trigger_tick(t, 3) == MINI_OS_OK);      /* inactive: not armed */
    drive_until(t0 + 40);
    CHECK(s_hard_count == 101);
    CHECK((t->flag & MINI_OS_TIMER_FLAG_ACTIVE) == 0);
    CHECK(mini_os_timer_set_trigger_tick(t, 0) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);
    drive_until(t0 + 60);
    CHECK(s_hard_count == 101);                                     /* deleted: silent */

    /* periodic: fixed-rate re-arm, 4 fires in 20 ticks */
    t = mini_os_timer_create("hard_p", hard_cb, MINI_OS_NULL, 5, 1, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + 20);
    CHECK(s_hard_count == 4);                                       /* ticks 5/10/15/20 */
    CHECK((t->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0);              /* stays armed */

    /* live period change: the old deadline vanishes, the new one is exact */
    CHECK(mini_os_timer_set_trigger_tick(t, 10) == MINI_OS_OK);     /* re-arms from t0+20 */
    drive_until(t0 + 29);
    CHECK(s_hard_count == 4);                                       /* old t0+25 fire is gone */
    drive_ticks(1);
    CHECK(s_hard_count == 5);                                       /* new fire at t0+30 */
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);
    drive_until(t0 + 60);
    CHECK(s_hard_count == 5);

    /* wheel boundary: one full revolution (tick == MINI_OS_TICK_WHEEL) */
    t = mini_os_timer_create("hard_w", hard_cb, MINI_OS_NULL,
                             MINI_OS_TICK_WHEEL, 0, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + MINI_OS_TICK_WHEEL - 1);
    CHECK(s_hard_count == 0);
    drive_ticks(1);
    CHECK(s_hard_count == 1);                                       /* fires at t0+WHEEL */
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);

    /* wheel boundary: one slot past a revolution (tick == WHEEL + 1, round 1) */
    t = mini_os_timer_create("hard_w1", hard_cb, MINI_OS_NULL,
                             MINI_OS_TICK_WHEEL + 1, 0, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + MINI_OS_TICK_WHEEL);
    CHECK(s_hard_count == 0);
    drive_ticks(1);
    CHECK(s_hard_count == 1);                                       /* fires at t0+WHEEL+1 */
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);

    /* shortest possible period: fire on the very next tick */
    t = mini_os_timer_create("hard_n1", hard_cb, MINI_OS_NULL, 1, 0, MINI_OS_TIMER_FLAG_HARD);
    CHECK(t != MINI_OS_NULL);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(t) == MINI_OS_OK);
    t0 = now();
    drive_ticks(1);
    CHECK(s_hard_count == 1);
    CHECK(mini_os_timer_delete(t) == MINI_OS_OK);

    /* static variant roundtrip */
    CHECK(mini_os_timer_create_static("hard_s", hard_cb, MINI_OS_NULL,
                                      2, 0, MINI_OS_TIMER_FLAG_HARD, &ts) == &ts);
    s_hard_count = 0;
    CHECK(mini_os_timer_start(&ts) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + 2);
    CHECK(s_hard_count == 1);
    CHECK(mini_os_timer_stop(&ts) == MINI_OS_OK);
    CHECK(mini_os_timer_delete_static(&ts) == MINI_OS_OK);
}

/* ================= 8. soft timer (timer service thread) =================== */
static volatile int s_soft_count = 0;

static void soft_cb(void *param)
{
    (void)param;
    s_soft_count++;
}

static void test_timer_soft(void)
{
    mini_os_timer_t *p;
    mini_os_timer_t *o;
    mini_os_timer_t *h;
    mini_os_tick_t t0;
    mini_os_size_t free_before;
    mini_os_size_t free_after;

    printf("--- soft timer: service thread, pending queue ---\n");

    /* the first SOFT start spawns the service thread at the idle level */
    p = mini_os_timer_create("soft_p", soft_cb, MINI_OS_NULL, 4, 1, MINI_OS_TIMER_FLAG_SOFT);
    CHECK(p != MINI_OS_NULL);
    s_soft_count = 0;
    CHECK(mini_os_timer_start(p) == MINI_OS_OK);

    /* expiry only queues the timer: nothing runs until the thread is served */
    t0 = now();
    drive_until(t0 + 4);
    CHECK(s_soft_count == 0);
    CHECK((p->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0);              /* pending */
    run_timer_thread();
    CHECK(s_soft_count == 1);
    CHECK((p->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0);              /* periodic re-armed */

    drive_until(t0 + 8);
    CHECK(s_soft_count == 1);                                       /* second expiry queued */
    run_timer_thread();
    CHECK(s_soft_count == 2);

    /* soft one-shot: served once, then deactivated */
    o = mini_os_timer_create("soft_1s", soft_cb, MINI_OS_NULL, 3, 0, MINI_OS_TIMER_FLAG_SOFT);
    CHECK(o != MINI_OS_NULL);
    CHECK(mini_os_timer_start(o) == MINI_OS_OK);
    drive_until(t0 + 11);                                           /* only o expires here */
    CHECK(s_soft_count == 2);
    run_timer_thread();
    CHECK(s_soft_count == 3);
    CHECK((o->flag & MINI_OS_TIMER_FLAG_ACTIVE) == 0);              /* one-shot done */
    drive_until(t0 + 12);                                           /* p re-armed at t0+8 */
    run_timer_thread();
    CHECK(s_soft_count == 4);

    /* stop while pending: the queued callback is dropped */
    CHECK(mini_os_timer_start(o) == MINI_OS_OK);                    /* re-arm the one-shot */
    drive_until(t0 + 15);
    CHECK((o->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0);              /* queued */
    CHECK(mini_os_timer_stop(o) == MINI_OS_OK);                     /* unlinked from pending */
    run_timer_thread();
    CHECK(s_soft_count == 4);                                       /* never served */
    CHECK(mini_os_timer_delete(o) == MINI_OS_OK);

    /* stop the periodic FIRST (it keeps re-firing every 4 ticks):
     * let p expire at t0+16 so it sits in the pending list, then stop it there */
    drive_until(t0 + 16);
    CHECK((p->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0);              /* queued */
    CHECK(mini_os_timer_stop(p) == MINI_OS_OK);                     /* unlinked from pending */
    drive_until(t0 + 20);
    run_timer_thread();                                             /* nothing left to serve */
    CHECK(s_soft_count == 4);
    CHECK((p->flag & MINI_OS_TIMER_FLAG_ACTIVE) == 0);
    CHECK(mini_os_timer_delete(p) == MINI_OS_OK);

    /* delete while pending (no other timer armed now): same drop, memory back */
    o = mini_os_timer_create("soft_del", soft_cb, MINI_OS_NULL, 3, 0, MINI_OS_TIMER_FLAG_SOFT);
    CHECK(o != MINI_OS_NULL);
    CHECK(mini_os_timer_start(o) == MINI_OS_OK);                    /* fires at t0+23 */
    drive_until(t0 + 23);
    free_before = mini_os_heap_free_space();
    CHECK(mini_os_timer_delete(o) == MINI_OS_OK);
    free_after = mini_os_heap_free_space();
    CHECK(free_after > free_before);
    run_timer_thread();
    CHECK(s_soft_count == 4);

    /* mixed hard + soft expiring in the same tick: hard runs at once, soft waits */
    s_hard_count = 0;
    h = mini_os_timer_create("mix_h", hard_cb, MINI_OS_NULL, 6, 0, MINI_OS_TIMER_FLAG_HARD);
    o = mini_os_timer_create("mix_s", soft_cb, MINI_OS_NULL, 6, 0, MINI_OS_TIMER_FLAG_SOFT);
    CHECK(h != MINI_OS_NULL && o != MINI_OS_NULL);
    CHECK(mini_os_timer_start(h) == MINI_OS_OK);
    CHECK(mini_os_timer_start(o) == MINI_OS_OK);
    t0 = now();
    drive_until(t0 + 6);
    CHECK(s_hard_count == 1);                                       /* inside the tick */
    CHECK(s_soft_count == 4);                                       /* queued, not run */
    run_timer_thread();
    CHECK(s_soft_count == 5);
    CHECK(mini_os_timer_delete(h) == MINI_OS_OK);
    CHECK(mini_os_timer_delete(o) == MINI_OS_OK);
}

/* ================== 9. thread time wheel (delay path) ====================== */
static volatile int s_delay_armed = 0;
static volatile mini_os_uint32_t s_until_target = 0;

static void delay_entry(void *param)
{
    (void)param;
    s_delay_armed = 1;
    (void)mini_os_thread_delay_tick(3);  /* parks; longjmps on the host */
    CHECK(!"delayed thread resumed mid-call (impossible on the host harness)");
}

static void delay_until_entry(void *param)
{
    (void)param;
    s_delay_armed = 1;
    (void)mini_os_thread_delay_tick_until(s_until_target);
}

static void test_wheel_delay(void)
{
    mini_os_thread_t *t;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;

    printf("--- thread time wheel: park, countdown, resume ---\n");

    /* relative delay: 3 ticks from now */
    t = mini_os_thread_create("delayer", 512, 6, delay_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    s_delay_armed = 0;
    run_thread_once(t);
    CHECK(s_delay_armed == 1);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
    CHECK(mini_os_wheel_remain(t) == 3);
    drive_ticks(2);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
    CHECK(mini_os_wheel_remain(t) == 1);
    drive_ticks(1);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
    CHECK(mini_os_wheel_remain(t) == 0);
    finish_thread(t);

    /* absolute deadline: 2 ticks ahead */
    t = mini_os_thread_create("until", 512, 6, delay_until_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    s_until_target = (mini_os_uint32_t)now() + 2;
    s_delay_armed = 0;
    run_thread_once(t);
    CHECK(s_delay_armed == 1);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);
    drive_ticks(2);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
    finish_thread(t);

    /* boundary: a deadline in the past returns without parking at all */
    t = mini_os_thread_create("past", 512, 6, delay_until_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    s_until_target = (mini_os_uint32_t)now() - 1;
    s_delay_armed = 0;
    run_thread_once(t);
    CHECK(s_delay_armed == 1);
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state != MINI_OS_THREAD_STATE_BLOCKED);
    finish_thread(t);
}

/* ================ 10. join retval + idle corpse reaping ==================== */
#if MINI_OS_THREAD_DETACH
static void * volatile s_join_retval = (void *)0xC0FFEEU;

static void exit_entry(void *param)
{
    (void)param;
    mini_os_thread_exit((void *)s_join_retval);   /* never returns: longjmps */
    CHECK(!"thread entry returned from exit");    /* unreached */
}

/* a second thread that polls the first one's retval (join needs a thread context) */
static mini_os_thread_t * volatile s_join_target = MINI_OS_NULL;
static void * volatile s_join_got = MINI_OS_NULL;
static volatile int s_join_ret = -1;

static void joiner_entry(void *param)
{
    (void)param;
    s_join_ret = mini_os_thread_join(s_join_target, (void **)(void *)&s_join_got, 0);
}

static jmp_buf g_idle_round;
static volatile int s_idle_hook_ran = 0;

static void idle_break(void *param)
{
    (void)param;
    s_idle_hook_ran = 1;
    longjmp(g_idle_round, 1);                     /* reaper already ran */
}

static void test_join_reap(void)
{
    mini_os_thread_t *t;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    mini_os_size_t free_before;
    mini_os_size_t free_after;

    printf("--- join retval + idle corpse reaping ---\n");
    t = mini_os_thread_create("join_me", 512, 6, exit_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        CHECK(mini_os_current_thread == t);
        t->entry(t->param);                       /* exits -> longjmp */
        CHECK(!"thread entry returned to scheduler");
    }
    CHECK(mini_os_thread_get_state(t, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_TERMINATED);

    /* fast path: already terminated, poll from a second thread's context */
    {
        mini_os_thread_t *joiner = mini_os_thread_create("joiner", 512, 5, joiner_entry, MINI_OS_NULL);

        CHECK(joiner != MINI_OS_NULL);
        s_join_target = t;
        s_join_ret = -1;
        s_join_got = MINI_OS_NULL;
        run_thread_once(joiner);
        CHECK(s_join_ret == MINI_OS_OK);
        CHECK(s_join_got == (void *)s_join_retval);
        finish_thread(joiner);
    }
    CHECK(mini_os_thread_join(MINI_OS_NULL, MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);

    /* the idle thread reaps the corpse (and exists at the lowest priority) */
    mini_os_thread_idle_create();
    CHECK(mini_os_thread_get_idle_handle() != MINI_OS_NULL);
    {
        mini_os_uint8_t idle_prio = 0;

        CHECK(mini_os_thread_get_priority(mini_os_thread_get_idle_handle(), &idle_prio) == MINI_OS_OK &&
              idle_prio == MINI_OS_PRIORITY - 1);
    }
    free_before = mini_os_heap_free_space();
    if (setjmp(g_idle_round) == 0)
    {
        (void)mini_os_thread_idle_hook(idle_break, MINI_OS_NULL); /* reaper -> hook -> longjmp */
        CHECK(!"idle loop returned without the hook");
    }
    CHECK(s_idle_hook_ran == 1);
    free_after = mini_os_heap_free_space();
    CHECK(free_after > free_before);              /* TCB + stack reclaimed */
}
#endif /* MINI_OS_THREAD_DETACH */

/* ======================= 11. find by name registry ========================= */
#if MINI_OS_FIND_BY_NAME
static void test_find_by_name(void)
{
    mini_os_thread_t *t;

    printf("--- find by name registry ---\n");
    CHECK(mini_os_find_by_name(MINI_OS_NULL) == MINI_OS_NULL);
    CHECK(mini_os_find_by_name("no_such_thread") == MINI_OS_NULL);
    t = mini_os_thread_create("named_t", 512, 9, noop_entry, MINI_OS_NULL);
    CHECK(t != MINI_OS_NULL);
    CHECK(mini_os_find_by_name("named_t") == t);
    finish_thread(t);
    CHECK(mini_os_find_by_name("named_t") == MINI_OS_NULL);         /* unregistered */
}
#endif /* MINI_OS_FIND_BY_NAME */

/* ================= 12. super stress: an 80-thread fleet ==================== */
/*
 * A "big project" modelled as SS_THREADS same-priority workers rotating over
 * SS_ROUNDS passes. Each pass runs one of eight primitive mixes (single,
 * ordered-pair, recursive and ISR-acquired mutexes; unbounded, binary and
 * bounded semaphores; queues; event groups) and then parks for one tick, so
 * the cooperative harness can hand its stack to the next thread. One HARD
 * timer keeps posting a semaphore from tick context while the fleet runs.
 *
 * Invariants checked continuously:
 *  - the per-thread model bitmap equals the kernel's hold_list after every op
 *    (no leaked lock and no phantom hold); worker 65 keeps one mutex held
 *    across all rounds, so every other worker really contends on it (AGAIN);
 *  - counters reconcile with the objects: final semaphore count ==
 *    initial + gives - takes, final queue depth == sends - receives;
 *  - a thread deleted or exiting while holding mutexes force-releases them;
 *  - SS_THREADS terminated TCBs are reclaimed by a single idle pass.
 *
 * Blocking parks (timeouts, PI waiters, join) belong to the earlier sections;
 * every call here is non-blocking, because the single-stack harness cannot
 * resume a C function that parked mid-call.
 */
/* quiet CHECK: the hot loops would otherwise flood the report */
#define CHECKQ(cond)                                                   \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            printf("FAIL %-58s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            failures++;                                                \
            fflush(stdout);                                            \
        }                                                              \
    } while (0)

#define SS_THREADS      80
#define SS_ROUNDS       24
#define SS_PRIO         1u
#define SS_MX_SHARED    5u                 /* freely taken and released */
#define SS_MX_ABANDON   5u                 /* index of the kept-held mutex */
#define SS_MX_TOKEN     6u                 /* the circulating single-unit token */
#define SS_MX_TOTAL     7u
#define SS_SEM_UNB      0u                 /* posted by the HARD timer */
#define SS_SEM_BOUND    5u
#define SS_SEM_RELAY    7u                 /* producer/consumer unit relay */
#define SS_SEM_TOTAL    8u
#define SS_Q_TOTAL      3u
#define SS_Q_SINK       2u
#define SS_EV_TOTAL     2u
#define SS_ABANDONER    65u
#define SS_SET_BITS     6u

#define SS_MX_BIT(n)    (1u << (mini_os_uint32_t)(n))

static mini_os_thread_t *s_ss_t[SS_THREADS];
static mini_os_mutex_t s_ss_mx[SS_MX_TOTAL];
static mini_os_mutex_t s_ss_rec_mx;         /* recursive */
static mini_os_mutex_t s_ss_kill_mx;        /* deleted-while-held victim */
static mini_os_semaphore_t s_ss_sem_store[SS_SEM_TOTAL];
static mini_os_semaphore_t *s_ss_sem[SS_SEM_TOTAL];
static mini_os_queue_t *s_ss_q[SS_Q_TOTAL];
#if MINI_OS_EVENT
static mini_os_event_group_t *s_ss_ev[SS_EV_TOTAL];
#endif
static mini_os_timer_t *s_ss_tick_timer;
static mini_os_uint32_t s_ss_hold[SS_THREADS];
static unsigned int s_ss_step[SS_THREADS];
static unsigned int s_ss_takes[SS_SEM_TOTAL];
static unsigned int s_ss_gives[SS_SEM_TOTAL];
static unsigned int s_ss_qsend[SS_Q_TOTAL];
static unsigned int s_ss_qrecv[SS_Q_TOTAL];
static unsigned int s_ss_ops;
static unsigned int s_ss_again;
static unsigned int s_ss_busy;
static unsigned int s_ss_hops;
static unsigned int s_ss_hop_dups;
static volatile int s_ss_live;
static volatile unsigned int s_ss_pass_owner = 0xFFFFFFFFu;
static volatile unsigned int s_ss_posts;
static const mini_os_uint16_t s_ss_sem_max[SS_SEM_TOTAL] =
{
    65535u, 1u, 1u, 1u, 1u, 4u, 8u, 4u
};
static const mini_os_uint16_t s_ss_sem_init[SS_SEM_TOTAL] =
{
    0u, 1u, 1u, 1u, 1u, 0u, 8u, 0u
};

/**
 * @brief Count the mutexes a thread really owns according to the kernel
 */
static unsigned int ss_hold_count(mini_os_thread_t *thread)
{
    mini_os_list_t *node;
    unsigned int n = 0;

    if (thread == MINI_OS_NULL)
    {
        return 0u;
    }
    for (node = thread->hold_list.next; node != &thread->hold_list; node = node->next)
    {
        n++;
    }
    return n;
}

/**
 * @brief The model says which mutexes worker `idx` holds right now
 */
static unsigned int ss_hold_expect(unsigned int idx)
{
    return (unsigned int)MINI_OS_POPCOUNT(s_ss_hold[idx]);
}

/* worker 65 leaves this mutex locked and dies holding it */
static void ss_hold_entry(void *param)
{
    (void)param;
    CHECKQ(mini_os_mutex_lock(&s_ss_kill_mx, 0) == MINI_OS_OK);
    /* park out of reach of the test horizon; the delete below must undo the lock */
    (void)mini_os_thread_delay_tick(0xFFFFFFFFu);
    CHECKQ(!"parked holder resumed");
}

/* HARD timer callback: posts the unbounded semaphore straight into the fleet */
static void ss_tick_cb(void *param)
{
    (void)param;
    if (mini_os_semaphore_give_isr(&s_ss_sem_store[SS_SEM_UNB]) == MINI_OS_OK)
    {
        s_ss_posts++;
    }
}

static void ss_sem_op(unsigned int idx, unsigned int s)
{
    mini_os_err_t err;
    mini_os_uint16_t cnt = 0;

    err = mini_os_semaphore_take(s_ss_sem[s], 0);
    if (err == MINI_OS_OK)
    {
        s_ss_takes[s]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }
    /* a saturated give is the double-release detector for binary sems */
    err = mini_os_semaphore_give(s_ss_sem[s]);
    if (err == MINI_OS_OK)
    {
        s_ss_gives[s]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_BUSY);
        s_ss_busy++;
    }
    err = mini_os_semaphore_give_isr(s_ss_sem[s]);
    if (err == MINI_OS_OK)
    {
        s_ss_gives[s]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_BUSY);
        s_ss_busy++;
    }
    err = mini_os_semaphore_try_take(s_ss_sem[s]);
    if (err == MINI_OS_OK)
    {
        s_ss_takes[s]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }
    CHECKQ(mini_os_semaphore_get_count(s_ss_sem[s], &cnt) == MINI_OS_OK);
    CHECKQ(cnt <= s_ss_sem_max[s]);
    /* a parking taker needs a unit: only the unbounded pool can be empty while
     * someone waits, and the harness has no resumable park, so probe INVAL */
    CHECKQ(mini_os_semaphore_take(MINI_OS_NULL, 0) == MINI_OS_ERR_INVAL);
    (void)idx;
}

static void ss_queue_op(unsigned int idx, unsigned int q, int v)
{
    mini_os_err_t err;
    int r = 0;

    err = mini_os_queue_send(s_ss_q[q], &v, 0);
    if (err == MINI_OS_OK)
    {
        s_ss_qsend[q]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);            /* full */
    }
    err = mini_os_queue_send_isr(s_ss_q[q], &v);
    if (err == MINI_OS_OK)
    {
        s_ss_qsend[q]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);            /* full: the ISR path never blocks */
    }
    err = mini_os_queue_receive(s_ss_q[q], &r, 0);
    if (err == MINI_OS_OK)
    {
        s_ss_qrecv[q]++;
        /* payload integrity: every word in a queue was put there by a worker,
         * encoded as idx * 100 + step, so both fields must stay in range */
        CHECKQ(r >= 0 && (unsigned int)r / 100u < (unsigned int)SS_THREADS &&
               (unsigned int)r % 100u < (unsigned int)SS_ROUNDS);
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);            /* empty */
    }
    err = mini_os_queue_receive_isr(s_ss_q[q], &r);
    if (err == MINI_OS_OK)
    {
        s_ss_qrecv[q]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }
    CHECKQ(mini_os_queue_get_depth(s_ss_q[q]) ==
           (mini_os_uint8_t)(s_ss_qsend[q] - s_ss_qrecv[q]));
    CHECKQ(mini_os_queue_send(MINI_OS_NULL, &v, 0) == MINI_OS_ERR_INVAL);
    (void)idx;
}

/**
 * @brief Circulate the single-unit token mutex across passes of the fleet
 * @details Whoever takes it keeps it over its park and releases it at the start
 *          of its next pass, so every other worker really contends on it while
 *          the owner is off the CPU. That proves two things the single-stack
 *          harness cannot otherwise see: the mutex unit is never duplicated
 *          (a taker must not be the current owner) and never lost (a holder
 *          that dies in its final pass hands it back through the force-release
 *          in mini_os_thread_exit()).
 */
static void ss_token_op(unsigned int idx)
{
    mini_os_err_t err;

    if ((s_ss_hold[idx] & SS_MX_BIT(SS_MX_TOKEN)) != 0u)
    {
        CHECKQ(s_ss_pass_owner == idx);                    /* nobody else took it */
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[SS_MX_TOKEN]) == MINI_OS_OK);
        s_ss_hold[idx] &= ~SS_MX_BIT(SS_MX_TOKEN);
        s_ss_pass_owner = 0xFFFFFFFFu;
    }

    err = mini_os_mutex_lock(&s_ss_mx[SS_MX_TOKEN], 0);
    if (err == MINI_OS_OK)
    {
        if (s_ss_pass_owner == idx)
        {
            s_ss_hop_dups++;                               /* the unit doubled */
        }
        CHECKQ(s_ss_pass_owner != idx);
        s_ss_pass_owner = idx;
        s_ss_hops++;
        s_ss_hold[idx] |= SS_MX_BIT(SS_MX_TOKEN);
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);                  /* a peer is holding it */
    }
}

/**
 * @brief Relay units between workers: gives and takes must cancel out exactly
 */
static void ss_relay_op(unsigned int idx, unsigned int step)
{
    mini_os_err_t err;

    if ((step & 1u) != 0u)
    {
        err = mini_os_semaphore_give(s_ss_sem[SS_SEM_RELAY]);
        if (err == MINI_OS_OK)
        {
            s_ss_gives[SS_SEM_RELAY]++;
        }
        else
        {
            CHECKQ(err == MINI_OS_ERR_BUSY);               /* relay saturated */
        }
    }
    else
    {
        err = mini_os_semaphore_take(s_ss_sem[SS_SEM_RELAY], 0);
        if (err == MINI_OS_OK)
        {
            s_ss_takes[SS_SEM_RELAY]++;
        }
        else
        {
            CHECKQ(err == MINI_OS_ERR_AGAIN);              /* nothing relayed yet */
        }
    }
    (void)idx;
}

static void ss_worker_entry(void *param)
{
    unsigned int idx = (unsigned int)(mini_os_size_t)param;
    unsigned int step = s_ss_step[idx];
    unsigned int variant = (idx + step) % 8u;
    mini_os_err_t err;
    unsigned int k;
    int v;

    s_ss_ops++;
    v = (int)(idx * 100u + step);

    ss_token_op(idx);
    ss_relay_op(idx, step);

    /* every pass first meets the mutex worker 65 keeps across all rounds */
    err = mini_os_mutex_lock(&s_ss_mx[SS_MX_ABANDON], 0);
    if (idx == SS_ABANDONER)
    {
        CHECKQ(err == MINI_OS_OK || err == MINI_OS_ERR_BUSY);
        if (err == MINI_OS_OK)
        {
            s_ss_hold[idx] |= SS_MX_BIT(SS_MX_ABANDON);
        }
    }
    else if (err == MINI_OS_OK)
    {
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[SS_MX_ABANDON]) == MINI_OS_OK); /* 65 is gone */
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);           /* contested, non-blocking */
        s_ss_again++;
    }

    if (variant == 0u || variant == 1u)
    {
        unsigned int m = (idx + step) % SS_MX_SHARED;

        CHECKQ(mini_os_mutex_lock(&s_ss_mx[m], 0) == MINI_OS_OK);
        s_ss_hold[idx] |= SS_MX_BIT(m);
        CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));
        CHECKQ(mini_os_mutex_lock(&s_ss_mx[m], 0) == MINI_OS_ERR_BUSY);  /* re-lock */
        CHECKQ(mini_os_mutex_unlock(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[m]) == MINI_OS_OK);
        s_ss_hold[idx] &= ~SS_MX_BIT(m);
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[m]) == MINI_OS_ERR_INVAL);  /* not the owner */
        CHECKQ(mini_os_mutex_lock(&s_ss_mx[m], 0) == MINI_OS_OK);        /* reacquire */
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[m]) == MINI_OS_OK);
    }
    else if (variant == 2u)
    {
        /* ordered pair: the global order is what makes this deadlock-free */
        unsigned int a = idx % SS_MX_SHARED;
        unsigned int b = (idx + 1u) % SS_MX_SHARED;
        unsigned int lo = (a < b) ? a : b;
        unsigned int hi = (a < b) ? b : a;

        CHECKQ(mini_os_mutex_lock(&s_ss_mx[lo], 0) == MINI_OS_OK);
        s_ss_hold[idx] |= SS_MX_BIT(lo);
        CHECKQ(mini_os_mutex_lock(&s_ss_mx[hi], 0) == MINI_OS_OK);
        s_ss_hold[idx] |= SS_MX_BIT(hi);
        CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));
        CHECKQ(mini_os_mutex_delete_static(&s_ss_mx[lo]) == MINI_OS_ERR_BUSY);  /* held */
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[hi]) == MINI_OS_OK);        /* unwound order */
        s_ss_hold[idx] &= ~SS_MX_BIT(hi);
        CHECKQ(mini_os_mutex_unlock(&s_ss_mx[lo]) == MINI_OS_OK);
        s_ss_hold[idx] &= ~SS_MX_BIT(lo);
        CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));
    }
    else if (variant == 3u)
    {
        /* one hold node, three deepening locks, then the full unwind */
        CHECKQ(mini_os_mutex_lock(&s_ss_rec_mx, 0) == MINI_OS_OK);
        s_ss_hold[idx] |= SS_MX_BIT(0);
        CHECKQ(mini_os_mutex_lock(&s_ss_rec_mx, 0) == MINI_OS_OK);
        CHECKQ(mini_os_mutex_lock(&s_ss_rec_mx, 0) == MINI_OS_OK);
        CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));
        CHECKQ(mini_os_mutex_unlock(&s_ss_rec_mx) == MINI_OS_OK);       /* depth 2 */
        CHECKQ(mini_os_mutex_lock(&s_ss_rec_mx, 0) == MINI_OS_OK);      /* still ours: 3 */
        CHECKQ(mini_os_mutex_unlock(&s_ss_rec_mx) == MINI_OS_OK);       /* 2 */
        CHECKQ(mini_os_mutex_unlock(&s_ss_rec_mx) == MINI_OS_OK);       /* 1 */
        CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));
        CHECKQ(mini_os_mutex_unlock(&s_ss_rec_mx) == MINI_OS_OK);       /* 0: released */
        s_ss_hold[idx] &= ~SS_MX_BIT(0);
        CHECKQ(mini_os_mutex_unlock(&s_ss_rec_mx) == MINI_OS_ERR_INVAL); /* no owner left */
        CHECKQ(mini_os_mutex_delete(&s_ss_rec_mx) == MINI_OS_ERR_NOTSUPP); /* static storage */
    }
    else if (variant == 4u)
    {
        unsigned int m = (idx + 2u) % SS_MX_SHARED;

        err = mini_os_mutex_lock_isr(&s_ss_mx[m]);
        CHECKQ(err == MINI_OS_OK || err == MINI_OS_ERR_AGAIN);
        if (err == MINI_OS_OK)
        {
            s_ss_hold[idx] |= SS_MX_BIT(m);
            CHECKQ(mini_os_mutex_unlock_isr(&s_ss_mx[m]) == MINI_OS_OK);
            s_ss_hold[idx] &= ~SS_MX_BIT(m);
            CHECKQ(mini_os_mutex_unlock_isr(&s_ss_mx[m]) == MINI_OS_ERR_INVAL);
        }
        CHECKQ(mini_os_mutex_lock_isr(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    }
    else if (variant == 5u)
    {
        ss_sem_op(idx, idx % SS_SEM_TOTAL);
        ss_sem_op(idx, SS_SEM_BOUND);
    }
    else if (variant == 6u)
    {
        ss_queue_op(idx, idx % SS_Q_TOTAL, v);
    }
    else
    {
#if MINI_OS_EVENT
        mini_os_uint32_t bits = 0;
        mini_os_uint32_t bit = (mini_os_uint32_t)1u << (idx % SS_SET_BITS);

        CHECKQ(mini_os_event_set_group(s_ss_ev[0], bit) == MINI_OS_OK);
        CHECKQ(mini_os_event_wait(s_ss_ev[0], bit, 0, &bits) == MINI_OS_OK);
        CHECKQ((bits & bit) != 0u);                /* the wait reported the bit */
        CHECKQ(mini_os_event_get_group(s_ss_ev[0], &bits) == MINI_OS_OK);
        CHECKQ((bits & bit) == 0u);                /* auto-clear consumed it */
        CHECKQ(mini_os_event_wait(s_ss_ev[0], (mini_os_uint32_t)1u << 30, 0, &bits) ==
               MINI_OS_ERR_AGAIN);                 /* nobody ever sets bit 30 */
        CHECKQ(mini_os_event_set_group(s_ss_ev[1], 0x3u) == MINI_OS_OK);
        CHECKQ(mini_os_event_wait(s_ss_ev[1], 0x30u, 0, &bits) == MINI_OS_ERR_AGAIN);
        CHECKQ(mini_os_event_wait(s_ss_ev[1], 0x3u, 0, &bits) == MINI_OS_OK);
#endif
        ss_queue_op(idx, SS_Q_SINK, v);
    }

    /* drain whatever the tick-context timer has posted (up to four units) */
    for (k = 0; k < 4u; k++)
    {
        err = mini_os_semaphore_take(&s_ss_sem_store[SS_SEM_UNB], 0);
        if (err != MINI_OS_OK)
        {
            CHECKQ(err == MINI_OS_ERR_AGAIN);
            break;
        }
        s_ss_takes[SS_SEM_UNB]++;
    }

    /* the model and the kernel must still agree when the pass is over */
    CHECKQ(ss_hold_count(mini_os_thread_current()) == ss_hold_expect(idx));

    s_ss_step[idx] = step + 1u;
    if (s_ss_step[idx] >= (unsigned int)SS_ROUNDS)
    {
        /* Dying while the abandon mutex and/or the circulating token are held:
         * mini_os_thread_exit() force-releases both, so the model drops
         * exactly those two bits here. */
        s_ss_hold[idx] &= ~(SS_MX_BIT(SS_MX_ABANDON) | SS_MX_BIT(SS_MX_TOKEN));
        s_ss_live--;
        mini_os_thread_exit(MINI_OS_NULL);
    }
    (void)mini_os_thread_delay_tick(1);
}

static jmp_buf g_ss_idle_round;

static void ss_idle_break(void *param)
{
    (void)param;
    longjmp(g_ss_idle_round, 1);
}

static void ss_run_next(void)
{
    mini_os_thread_t * volatile picked = MINI_OS_NULL;

    if (mini_os_get_highest_priority() != SS_PRIO)
    {
        return;                                   /* no worker ready */
    }
    if (setjmp(g_sched) == 0)
    {
        CHECK(mini_os_schedule_switch() == MINI_OS_OK);
        picked = mini_os_current_thread;
        CHECKQ(picked != MINI_OS_NULL && picked->priority == SS_PRIO);
        if (picked != MINI_OS_NULL && picked->priority == SS_PRIO)
        {
            picked->entry(picked->param);
        }
    }
}

static void test_super_stress(void)
{
    unsigned int i;
    mini_os_uint8_t k;
    mini_os_uint16_t cnt = 0;
    mini_os_uint32_t held_left;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    mini_os_size_t free_before;
    mini_os_size_t free_after;

    printf("--- super stress: %d workers x %d rounds ---\n", SS_THREADS, SS_ROUNDS);

    for (i = 0; i < SS_MX_TOTAL; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "ss_mx%u", i);
        CHECK(mini_os_mutex_create_static(&s_ss_mx[i], nm) == &s_ss_mx[i]);
    }
    CHECK(mini_os_mutex_recuring_create_static("ss_rec", &s_ss_rec_mx) == &s_ss_rec_mx);
    CHECK(mini_os_mutex_create_static(&s_ss_kill_mx, "ss_kill") == &s_ss_kill_mx);
    for (i = 0; i < SS_SEM_TOTAL; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "ss_sem%u", i);
        s_ss_sem[i] = mini_os_semaphore_create_static(nm, s_ss_sem_max[i], s_ss_sem_init[i],
                                                      &s_ss_sem_store[i]);
        CHECK(s_ss_sem[i] == &s_ss_sem_store[i]);
    }
    for (i = 0; i < SS_Q_TOTAL; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "ss_q%u", i);
        s_ss_q[i] = mini_os_queue_create(nm, (int)sizeof(int), 16);
        CHECK(s_ss_q[i] != MINI_OS_NULL);
    }
#if MINI_OS_EVENT
    for (i = 0; i < SS_EV_TOTAL; i++)
    {
        s_ss_ev[i] = mini_os_event_group_create(0, MINI_OS_EVENT_OR_TYPE);   /* heap owned */
        CHECK(s_ss_ev[i] != MINI_OS_NULL);
    }
#endif

    /* a thread deleted while it holds a mutex must leave that mutex free */
    {
        mini_os_thread_t *holder = mini_os_thread_create("ss_holder",
                                                         MINI_OS_THREAD_MIN_STACK_SIZE,
                                                         SS_PRIO, ss_hold_entry, MINI_OS_NULL);

        CHECK(holder != MINI_OS_NULL);
        ss_run_next();
        reset_current();
        CHECK(mini_os_thread_delete(holder) == MINI_OS_OK);   /* parked + holding */
        CHECK(mini_os_mutex_unlock(&s_ss_kill_mx) == MINI_OS_ERR_INVAL); /* not the owner */
        CHECK(mini_os_mutex_delete_static(&s_ss_kill_mx) == MINI_OS_OK);       /* force-released */
    }

    /* the tick-context producer for the fleet */
    s_ss_tick_timer = mini_os_timer_create("ss_tick", ss_tick_cb, MINI_OS_NULL, 3, 1,
                                           MINI_OS_TIMER_FLAG_HARD);
    CHECK(s_ss_tick_timer != MINI_OS_NULL);
    CHECK(mini_os_timer_start(s_ss_tick_timer) == MINI_OS_OK);

    for (i = 0; i < SS_THREADS; i++)
    {
        char nm[16];

        (void)snprintf(nm, sizeof(nm), "ss_%u", i);
        s_ss_t[i] = mini_os_thread_create(nm, MINI_OS_THREAD_MIN_STACK_SIZE, SS_PRIO,
                                          ss_worker_entry, (void *)(mini_os_size_t)i);
        CHECKQ(s_ss_t[i] != MINI_OS_NULL);
        if (s_ss_t[i] != MINI_OS_NULL)
        {
            s_ss_live++;
        }
    }
    CHECK(s_ss_live == SS_THREADS);
    CHECK(mini_os_heap_free_space() > 0);          /* the fleet fitted in the heap */

    for (i = 0; i < (unsigned int)SS_ROUNDS; i++)
    {
        unsigned int runs = 0;

        drive_ticks(1);                            /* wake last round's parks */
        while (runs < (unsigned int)SS_THREADS)
        {
            mini_os_uint8_t before = mini_os_get_highest_priority();

            if (before != SS_PRIO)
            {
                break;
            }
            ss_run_next();
            runs++;
        }
    }
    reset_current();

    printf("  fleet ran %u passes, %u contended mutex attempts, %u saturated gives\n",
           s_ss_ops, s_ss_again, s_ss_busy);
    printf("  token circulated %u hops (%d duplicates), relay: %u gives / %u takes\n",
           s_ss_hops, (int)s_ss_hop_dups, s_ss_gives[SS_SEM_RELAY], s_ss_takes[SS_SEM_RELAY]);
    CHECK(s_ss_ops == (unsigned int)SS_THREADS * (unsigned int)SS_ROUNDS);
    CHECK(s_ss_live == 0);
    CHECK(s_ss_posts > 0);

    for (i = 0; i < SS_THREADS; i++)
    {
        CHECKQ(s_ss_step[i] == (unsigned int)SS_ROUNDS);
        CHECKQ(s_ss_hold[i] == 0u);
        CHECKQ(mini_os_thread_get_state(s_ss_t[i], &state) == MINI_OS_OK &&
               state == MINI_OS_THREAD_STATE_TERMINATED);
    }

    /* semaphore and queue bookkeeping must reconcile with the op counters */
    for (i = 0; i < SS_SEM_TOTAL; i++)
    {
        unsigned int expect = (unsigned int)s_ss_sem_init[i] + s_ss_gives[i] - s_ss_takes[i];

        if (i == SS_SEM_UNB)
        {
            expect += s_ss_posts;
        }
        CHECK(mini_os_semaphore_get_count(s_ss_sem[i], &cnt) == MINI_OS_OK);
        CHECKQ((unsigned int)cnt == expect);
    }
    for (i = 0; i < SS_Q_TOTAL; i++)
    {
        CHECKQ(mini_os_queue_get_depth(s_ss_q[i]) ==
               (mini_os_uint8_t)(s_ss_qsend[i] - s_ss_qrecv[i]));
    }

    /* the abandoner died with it locked: the kernel had to hand it back */
    held_left = s_ss_mx[SS_MX_ABANDON].semaphore.count;
    CHECK(held_left == 1u);
    CHECK(s_ss_mx[SS_MX_ABANDON].owner == MINI_OS_NULL);
    CHECK(mini_os_mutex_delete_static(&s_ss_mx[SS_MX_ABANDON]) == MINI_OS_OK);

    /* the circulating token was never duplicated and always came back, even
     * when the holder died while it still owned it */
    CHECK(s_ss_hop_dups == 0u);
    CHECK(s_ss_hops > 1u);
    CHECK(s_ss_mx[SS_MX_TOKEN].semaphore.count == 1u);
    CHECK(s_ss_mx[SS_MX_TOKEN].owner == MINI_OS_NULL);
    CHECK(s_ss_gives[SS_SEM_RELAY] >= s_ss_takes[SS_SEM_RELAY]);
    CHECK(mini_os_semaphore_get_count(s_ss_sem[SS_SEM_RELAY], &cnt) == MINI_OS_OK);
    CHECK((unsigned int)cnt == s_ss_gives[SS_SEM_RELAY] - s_ss_takes[SS_SEM_RELAY]);

#if MINI_OS_EVENT
    {
        mini_os_uint32_t bits = 0;

        /* every bit any worker set was consumed by that same pass */
        CHECK(mini_os_event_get_group(s_ss_ev[0], &bits) == MINI_OS_OK);
        CHECK(bits == 0u);
        CHECK(mini_os_event_get_group(s_ss_ev[1], &bits) == MINI_OS_OK);
        CHECK(bits == 0u);
    }
    CHECK(mini_os_event_group_delete(s_ss_ev[1]) == MINI_OS_OK);
    CHECK(mini_os_event_group_delete(s_ss_ev[0]) == MINI_OS_OK);
#endif

    CHECK(mini_os_timer_stop(s_ss_tick_timer) == MINI_OS_OK);
    CHECK(mini_os_timer_delete(s_ss_tick_timer) == MINI_OS_OK);
    for (k = 0; k < SS_MX_SHARED; k++)
    {
        CHECK(mini_os_mutex_delete_static(&s_ss_mx[k]) == MINI_OS_OK);
    }
    CHECK(mini_os_mutex_delete_static(&s_ss_mx[SS_MX_TOKEN]) == MINI_OS_OK);
    CHECK(mini_os_mutex_delete_static(&s_ss_rec_mx) == MINI_OS_OK);
    for (i = 0; i < SS_SEM_TOTAL; i++)
    {
        CHECK(mini_os_semaphore_delete_static(s_ss_sem[i]) == MINI_OS_OK);
    }
    for (i = 0; i < SS_Q_TOTAL; i++)
    {
        CHECK(mini_os_queue_delete(s_ss_q[i]) == MINI_OS_OK);
    }

    /* one idle pass reclaims all 80 corpses */
    free_before = mini_os_heap_free_space();
    if (setjmp(g_ss_idle_round) == 0)
    {
        (void)mini_os_thread_idle_hook(ss_idle_break, MINI_OS_NULL);
        CHECK(!"idle loop returned without the hook");
    }
    free_after = mini_os_heap_free_space();
    printf("  reclaimed heap: %lu bytes back\n", (unsigned long)(free_after - free_before));
    CHECK(free_after > free_before + 20000u);
}

/* ============ 13. super timer module: 30 timers, exact counts ============== */
/*
 * Every wheel shape at once: periods below, exactly at and far above the wheel
 * size (round math), HARD and SOFT, one-shot and periodic, callbacks that stop
 * or delete or re-arm themselves (the use-after-free the HARD path was fixed
 * for), one that re-programs its own period mid-flight, and a NULL-callback
 * guard. Each model carries its exact expected fire count over ST_SPAN ticks,
 * so a single lost, duplicated or late fire is a failure.
 */
#define ST_COUNT 30
#define ST_SPAN  100u

typedef struct
{
    mini_os_tick_t   period;
    mini_os_uint8_t  num;                  /* MINI_OS_TIMER_FLAG_ONE_SHOT / _PERIODIC */
    mini_os_uint8_t  mode;                 /* MINI_OS_TIMER_FLAG_HARD / _SOFT */
    unsigned int     expect;
    const char      *what;
} ss_st_model_t;

static const ss_st_model_t s_st_model[ST_COUNT] =
{
    {  1u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD, 100u, "every tick" },
    { 32u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   3u, "wheel exact" },
    { 33u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   3u, "wheel+1" },
    { 64u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   1u, "2*wheel" },
    { 65u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   1u, "2*wheel+1" },
    {  2u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,  50u, "period 2" },
    {  2u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,  50u, "soft 2" },
    {  3u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,  33u, "soft 3" },
    {  7u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,  14u, "cross slot" },
    {  7u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,  14u, "soft cross" },
    { 13u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   7u, "soft 13" },
    { 16u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   6u, "half wheel" },
    { 16u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   6u, "soft half" },
    { 31u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   3u, "wheel-1" },
    { 31u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   3u, "soft w-1" },
    { 50u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   2u, "period 50" },
    {101u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   0u, "never in span" },
    {101u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   0u, "soft never" },
    {  1u, MINI_OS_TIMER_FLAG_ONE_SHOT, MINI_OS_TIMER_FLAG_HARD,   1u, "one tick off" },
    { 50u, MINI_OS_TIMER_FLAG_ONE_SHOT, MINI_OS_TIMER_FLAG_HARD,   1u, "mid one-shot" },
    {100u, MINI_OS_TIMER_FLAG_ONE_SHOT, MINI_OS_TIMER_FLAG_HARD,   1u, "last tick" },
    {100u, MINI_OS_TIMER_FLAG_ONE_SHOT, MINI_OS_TIMER_FLAG_SOFT,   1u, "last soft" },
    {101u, MINI_OS_TIMER_FLAG_ONE_SHOT, MINI_OS_TIMER_FLAG_HARD,   0u, "past span" },
    {  5u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   3u, "cb stops self" },
    {  6u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   3u, "soft stops" },
    {  9u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,   1u, "cb deletes self" },
    { 11u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   1u, "soft deletes" },
    {  8u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,  12u, "cb restarts self" },
    { 12u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_SOFT,   6u, "cb reperiods" },
    {  4u, MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD,  25u, "churn create+del" }
};

static mini_os_timer_t *s_st_t[ST_COUNT];
static int s_st_fire[ST_COUNT];
static int s_st_reprog;
static mini_os_thread_t *s_st_svc;

static void ss_st_cb(void *param)
{
    unsigned int i = (unsigned int)(mini_os_size_t)param;

    s_st_fire[i]++;

    if (i == 23u || i == 24u)
    {
        /* stop from inside the callback: must kill every later fire even though
         * the HARD path already re-armed this timer before calling us */
        if ((unsigned)s_st_fire[i] >= 3u)
        {
            (void)mini_os_timer_stop(s_st_t[i]);
        }
    }
    else if (i == 25u || i == 26u)
    {
        /* delete this very timer: the tick/drain loop may not touch it again */
        mini_os_timer_t *self = s_st_t[i];

        s_st_t[i] = MINI_OS_NULL;
        (void)mini_os_timer_delete(self);
    }
    else if (i == 27u)
    {
        /* restart mid callback: re-arms from now, same fixed rate */
        (void)mini_os_timer_start(s_st_t[i]);
    }
    else if (i == 28u)
    {
        if (s_st_fire[i] == 2)
        {
            s_st_reprog = 1;
            (void)mini_os_timer_set_trigger_tick(s_st_t[i], 16);
        }
    }
    else if (i == 29u)
    {
        /* heap churn in callback context: build and drop a silent one-shot */
        mini_os_timer_t *tmp = mini_os_timer_create("ss_churn", ss_st_cb, (void *)(mini_os_size_t)99u,
                                                    1000, 0, MINI_OS_TIMER_FLAG_HARD);

        if (tmp != MINI_OS_NULL)
        {
            (void)mini_os_timer_delete(tmp);
        }
    }
}

/* Enter the SOFT timer service thread directly: the service thread no longer
 * shares the lowest priority with idle (it sits one step above), but a plain
 * switch pick could still hand the harness stack to some other READY thread,
 * so the entry is forced and gated on the service being READY. */
static void ss_run_soft_service(void)
{
    if (s_st_svc == MINI_OS_NULL || s_st_svc->state != MINI_OS_THREAD_STATE_READY)
    {
        return;
    }
    if (setjmp(g_sched) == 0)
    {
        mini_os_current_thread = s_st_svc;
        s_st_svc->entry(s_st_svc->param);
    }
    mini_os_current_thread = MINI_OS_NULL;
}

static void test_super_timers(void)
{
    unsigned int i;
    unsigned int served;
    mini_os_size_t free_before;
    mini_os_size_t free_after;

    printf("--- super timer module: %d timers over %u ticks ---\n", ST_COUNT, ST_SPAN);

    for (i = 0; i < ST_COUNT; i++)
    {
        s_st_fire[i] = 0;
    }
    s_st_reprog = 0;

#if MINI_OS_FIND_BY_NAME
    s_st_svc = mini_os_find_by_name(MINI_OS_TIMER_THREAD_NAME);
#else
    s_st_svc = MINI_OS_NULL;
#endif
    served = (s_st_svc != MINI_OS_NULL) ? 1u : 0u;

    free_before = mini_os_heap_free_space();
    for (i = 0; i < ST_COUNT; i++)
    {
        char nm[16];

        (void)snprintf(nm, sizeof(nm), "ss_st%u", i);
        s_st_t[i] = mini_os_timer_create(nm, ss_st_cb, (void *)(mini_os_size_t)i,
                                         s_st_model[i].period, s_st_model[i].num,
                                         s_st_model[i].mode);
        CHECKQ(s_st_t[i] != MINI_OS_NULL);
        if (s_st_t[i] != MINI_OS_NULL)
        {
            CHECKQ(mini_os_timer_start(s_st_t[i]) == MINI_OS_OK);
            CHECKQ((s_st_t[i]->flag & MINI_OS_TIMER_FLAG_ACTIVE) != 0u);
        }
    }

    for (i = 0; i < ST_SPAN; i++)
    {
        drive_ticks(1);
        ss_run_soft_service();
    }

    for (i = 0; i < ST_COUNT; i++)
    {
        unsigned int want = s_st_model[i].expect;

        if (served == 0u && (s_st_model[i].mode & MINI_OS_TIMER_FLAG_SOFT) != 0u)
        {
            want = 0u;            /* no service thread to run soft callbacks */
        }
        printf("  timer[%2u] %-16s p=%3u %-4s %-4s expect=%3u got=%3d %s\n",
               i, s_st_model[i].what, (unsigned)s_st_model[i].period,
               (s_st_model[i].num == MINI_OS_TIMER_FLAG_PERIODIC) ? "per" : "once",
               (s_st_model[i].mode & MINI_OS_TIMER_FLAG_SOFT) ? "soft" : "hard",
               want, s_st_fire[i], ((unsigned)s_st_fire[i] == want) ? "ok" : "MISMATCH");
        CHECKQ((unsigned)s_st_fire[i] == want);
    }
    CHECK(s_st_reprog == 1);

    /* everything that survived must be unlinkable and deletable */
    for (i = 0; i < ST_COUNT; i++)
    {
        if (s_st_t[i] == MINI_OS_NULL)
        {
            continue;                                /* callback deleted itself */
        }
        CHECKQ(mini_os_timer_stop(s_st_t[i]) == MINI_OS_OK);
        CHECKQ((s_st_t[i]->flag & MINI_OS_TIMER_FLAG_ACTIVE) == 0u);
        CHECKQ(mini_os_timer_delete(s_st_t[i]) == MINI_OS_OK);
        s_st_t[i] = MINI_OS_NULL;
    }
    drive_ticks(1);
    ss_run_soft_service();                           /* nothing queued any more */

    free_after = mini_os_heap_free_space();
    printf("  timer heap: %lu bytes still in use\n",
           (unsigned long)(free_before - free_after));
    CHECK(free_after + 64u >= free_before);          /* all descriptors returned */

    /* boundaries the fleet could not reach: an inactive timer never re-arms */
    {
        mini_os_timer_t *t = mini_os_timer_create("ss_last", ss_st_cb, MINI_OS_NULL,
                                                  4, 1, MINI_OS_TIMER_FLAG_HARD);

        CHECK(t != MINI_OS_NULL);
        CHECK(mini_os_timer_start(t) == MINI_OS_OK);
        CHECK(mini_os_timer_stop(t) == MINI_OS_OK);
        CHECK(mini_os_timer_start(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
        CHECK(mini_os_timer_stop(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
        CHECK(mini_os_timer_delete(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
        CHECK(mini_os_timer_set_trigger_tick(MINI_OS_NULL, 4) == MINI_OS_ERR_INVAL);
        CHECK(mini_os_timer_set_callback(t, MINI_OS_NULL, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
        drive_ticks(20);
        CHECK(s_st_fire[0] == 100u);                 /* stopped timer stayed quiet */
        CHECK(mini_os_timer_delete(t) == MINI_OS_OK);
    }
}

/* ============ 14. priority sweep: every level, PI chains, ISR storm ========= */
/*
 * Tasks at EVERY application priority (0..30; 31 belongs to the idle reaper -
 * a same-priority tie could hand the harness stack to its endless loop). The
 * sweep round-robins the priorities: each pass locks two of the shared
 * mutexes, keeps one across its park (so rivals at other priorities really
 * contend), does semaphore and queue work with full accounting, then parks.
 * Between two rounds an interrupt storm fires from "ISR" context: give_isr,
 * send_isr, lock_isr (attributed to a parked stand-in thread),
 * set_group_isr and a period-1 HARD timer, plus the yield_isr contract in
 * both directions (no more urgent thread: returns; prio 0 ready: switches).
 * On top of the sweep two dedicated scenarios run:
 *  - a three-deep priority-inheritance chain with staged timeouts, proving
 *    the boost travels across two mutexes AND unwinds level by level;
 *  - a holder of THREE mutexes dying with a waiter parked on each, proving
 *    the kill path drains every queue and hands every unit back exactly once.
 */
#define SW_TASKS       31                 /* priorities 0..30 */
#define SW_MX          6u                 /* mutexes swept with a kept-held rotation */
#define SW_SEM_UNB     0u                 /* fed by the storm timer and the tasks */
#define SW_SEM_BIN1    1u
#define SW_SEM_BIN2    2u
#define SW_SEM_BIN3    3u
#define SW_SEM_BOUND   4u
#define SW_SEM_TOTAL   5u
#define SW_Q_TOTAL     2u
#define SW_ROUNDS      8u
#define SW_STORM_ITERS 24u                /* ISR iterations between two rounds */
#define SW_MX_BIT(n)   (1u << (mini_os_uint32_t)(n))

static mini_os_thread_t *s_sw_t[SW_TASKS];
static mini_os_thread_t *s_sw_isr_host;         /* parked stand-in for "the interrupted thread" */
static mini_os_mutex_t *s_sw_mx[SW_MX];
static mini_os_mutex_t *s_sw_isr_mx_a;          /* acquired/released via lock_isr */
static mini_os_mutex_t *s_sw_kill3[3];          /* the triple holder scenario */
static mini_os_mutex_t *s_sw_chain_outer;       /* held by the low chain thread */
static mini_os_mutex_t *s_sw_chain_inner;       /* held by the mid chain thread */
static mini_os_semaphore_t *s_sw_sem[SW_SEM_TOTAL];
static mini_os_queue_t *s_sw_q[SW_Q_TOTAL];
#if MINI_OS_EVENT
static mini_os_event_group_t *s_sw_ev;          /* bit accumulator for the storm */
#endif
static mini_os_timer_t *s_sw_storm_timer;       /* period 1 HARD: one post per tick */
static mini_os_uint32_t s_sw_hold[SW_TASKS];    /* the model: which mutexes each worker holds */
static unsigned int s_sw_step[SW_TASKS];
static unsigned int s_sw_takes[SW_SEM_TOTAL];
static unsigned int s_sw_gives[SW_SEM_TOTAL];
static unsigned int s_sw_gisr[SW_SEM_TOTAL];    /* give_isr successes */
static unsigned int s_sw_posts;                 /* storm timer cb posts */
static unsigned int s_sw_qsend[SW_Q_TOTAL];
static unsigned int s_sw_qrecv[SW_Q_TOTAL];
static unsigned int s_sw_qseq;                  /* monotonically increasing payload */
static volatile int s_sw_phase;                 /* chain scenario phase */
static volatile int s_sw_isr_holds_a;           /* does the stand-in own isr_mx_a right now */
static const mini_os_uint16_t s_sw_sem_max[SW_SEM_TOTAL] = { 65535u, 1u, 1u, 1u, 4u };
static const mini_os_uint16_t s_sw_sem_init[SW_SEM_TOTAL] = { 0u, 1u, 1u, 1u, 0u };
static unsigned int s_sw_again;                 /* cross-priority contention hits */

static void sw_worker_entry(void *param);       /* forward: the pump checks the entry */
static void sw_park_entry(void *param);         /* forward: the ISR stand-in's first run */

static void sw_storm_cb(void *param)
{
    (void)param;
    if (mini_os_semaphore_give_isr(s_sw_sem[SW_SEM_UNB]) == MINI_OS_OK)
    {
        s_sw_posts++;
    }
}

/* first mutex currently held by any fleet worker (SW_MX when none) */
static unsigned int sw_any_held_mutex(void)
{
    unsigned int m;
    unsigned int t;

    for (m = 0; m < SW_MX; m++)
    {
        for (t = 0; t < SW_TASKS; t++)
        {
            if ((s_sw_hold[t] & SW_MX_BIT(m)) != 0u)
            {
                return m;
            }
        }
    }
    return SW_MX;
}

/* one scheduling step: run exactly one fleet pass. The timer service thread
 * (prio 30) may wake up between two passes and share the ready list with the
 * fleet's own prio-30 worker: serve it, it re-parks, then pick again. */
static void sw_run_next(void)
{
    volatile int served = 0;

    while (served == 0)
    {
        if (setjmp(g_sched) == 0)
        {
            CHECK(mini_os_schedule_switch() == MINI_OS_OK);
            CHECK(mini_os_current_thread != MINI_OS_NULL);
            if (mini_os_current_thread->entry == sw_worker_entry)
            {
                served = 1;
            }
            else if (mini_os_current_thread->entry == sw_park_entry ||
                     mini_os_current_thread->priority == MINI_OS_TIMER_THREAD_PRIORITY)
            {
                /* legal non-fleet services: the ISR stand-in's first run
                 * (it parks itself out of reach) and the soft-timer service
                 * thread woken between two passes */
            }
            else
            {
                CHECKQ(!"sweep pump picked an unexpected thread");
            }
            mini_os_current_thread->entry(mini_os_current_thread->param);
        }
    }
}

static void sw_worker_entry(void *param)
{
    unsigned int idx = (unsigned int)(mini_os_size_t)param;
    unsigned int step = s_sw_step[idx];
    unsigned int a = idx % SW_MX;
    unsigned int b = (idx + 3u) % SW_MX;
    unsigned int keep = ((step & 1u) != 0u) ? a : b;
    unsigned int drop = (keep == a) ? b : a;
    unsigned int m;
    mini_os_err_t err;
    int v;
    int r;

    /* give back whatever the previous pass kept across its park */
    for (m = 0; m < SW_MX; m++)
    {
        if ((s_sw_hold[idx] & SW_MX_BIT(m)) != 0u)
        {
            CHECKQ(mini_os_mutex_unlock(s_sw_mx[m]) == MINI_OS_OK);
            s_sw_hold[idx] &= ~SW_MX_BIT(m);
        }
    }

    /* two mutexes, 0 timeout: contention from another priority shows as AGAIN */
    err = mini_os_mutex_lock(s_sw_mx[a], 0);
    if (err == MINI_OS_OK)
    {
        s_sw_hold[idx] |= SW_MX_BIT(a);
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
        s_sw_again++;
    }
    err = mini_os_mutex_lock(s_sw_mx[b], 0);
    if (err == MINI_OS_OK)
    {
        s_sw_hold[idx] |= SW_MX_BIT(b);
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
        s_sw_again++;
    }
    CHECKQ(ss_hold_count(mini_os_thread_current()) ==
           (unsigned int)MINI_OS_POPCOUNT(s_sw_hold[idx]));

    /* semaphore: take one class, give another (binaries saturate -> BUSY) */
    err = mini_os_semaphore_take(s_sw_sem[idx % SW_SEM_TOTAL], 0);
    if (err == MINI_OS_OK)
    {
        s_sw_takes[idx % SW_SEM_TOTAL]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }
    err = mini_os_semaphore_give(s_sw_sem[(idx + 1u) % SW_SEM_TOTAL]);
    if (err == MINI_OS_OK)
    {
        s_sw_gives[(idx + 1u) % SW_SEM_TOTAL]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_BUSY);
    }

    /* queue: send from "interrupt" context, receive in thread context */
    v = (int)(idx * 1000u + step);
    err = mini_os_queue_send_isr(s_sw_q[idx % SW_Q_TOTAL], &v);
    if (err == MINI_OS_OK)
    {
        s_sw_qsend[idx % SW_Q_TOTAL]++;
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }
    err = mini_os_queue_receive(s_sw_q[(idx + 1u) % SW_Q_TOTAL], &r, 0);
    if (err == MINI_OS_OK)
    {
        s_sw_qrecv[(idx + 1u) % SW_Q_TOTAL]++;
        CHECKQ(r / 1000 < SW_TASKS || r >= 50000);  /* worker- or ISR-encoded */
        if (r < 50000)
        {
            CHECKQ(r % 1000 < (int)SW_ROUNDS);
        }
    }
    else
    {
        CHECKQ(err == MINI_OS_ERR_AGAIN);
    }

    /* keep exactly one mutex across the park so the next pass of its rivals
     * sees real contention; the very last pass releases everything */
    if (step + 1u < SW_ROUNDS)
    {
        if ((s_sw_hold[idx] & SW_MX_BIT(drop)) != 0u)
        {
            CHECKQ(mini_os_mutex_unlock(s_sw_mx[drop]) == MINI_OS_OK);
            s_sw_hold[idx] &= ~SW_MX_BIT(drop);
        }
    }
    else
    {
        for (m = 0; m < SW_MX; m++)
        {
            if ((s_sw_hold[idx] & SW_MX_BIT(m)) != 0u)
            {
                CHECKQ(mini_os_mutex_unlock(s_sw_mx[m]) == MINI_OS_OK);
                s_sw_hold[idx] &= ~SW_MX_BIT(m);
            }
        }
    }
    CHECKQ(ss_hold_count(mini_os_thread_current()) ==
           (unsigned int)MINI_OS_POPCOUNT(s_sw_hold[idx]));

    s_sw_step[idx]++;
    (void)mini_os_thread_delay_tick(1);        /* park: the pump moves on */
}

/* one storm iteration: everything below runs from "interrupt" context */
static void sw_isr_storm(void)
{
    unsigned int k;
    unsigned int m;
    int v;
    volatile int no_switch;

    for (k = 0; k < SW_STORM_ITERS; k++)
    {
        mini_os_err_t err;

        /* semaphores: the binaries saturate (BUSY), the bounded one fills up */
        err = mini_os_semaphore_give_isr(s_sw_sem[SW_SEM_BIN1 + (k % 3u)]);
        if (err == MINI_OS_OK)
        {
            s_sw_gisr[SW_SEM_BIN1 + (k % 3u)]++;
        }
        else
        {
            CHECKQ(err == MINI_OS_ERR_BUSY);
        }
        err = mini_os_semaphore_give_isr(s_sw_sem[SW_SEM_BOUND]);
        if (err == MINI_OS_OK)
        {
            s_sw_gisr[SW_SEM_BOUND]++;
        }
        else
        {
            CHECKQ(err == MINI_OS_ERR_BUSY);
        }

        /* queue from interrupt context until it is full (AGAIN) */
        /* ISR payloads carry a base no worker can ever produce (max 30*1000+7) */
        v = (int)(50000u + s_sw_qseq++);
        err = mini_os_queue_send_isr(s_sw_q[k % SW_Q_TOTAL], &v);
        if (err == MINI_OS_OK)
        {
            s_sw_qsend[k % SW_Q_TOTAL]++;
        }
        else
        {
            CHECKQ(err == MINI_OS_ERR_AGAIN);
        }

        /* mutex: ownership is attributed to the interrupted thread */
        mini_os_current_thread = s_sw_isr_host;
        if ((k % 3u) == 0u)
        {
            CHECKQ(mini_os_mutex_lock_isr(s_sw_isr_mx_a) == MINI_OS_OK);
            s_sw_isr_holds_a = 1;
        }
        else if ((k % 3u) == 1u)
        {
            CHECKQ(mini_os_mutex_lock_isr(s_sw_isr_mx_a) == MINI_OS_ERR_BUSY);
        }
        else
        {
            CHECKQ(mini_os_mutex_unlock_isr(s_sw_isr_mx_a) == MINI_OS_OK);
            s_sw_isr_holds_a = 0;
        }
        /* a mutex owned by somebody else: an ISR neither blocks nor boosts */
        m = sw_any_held_mutex();
        if (m != SW_MX)
        {
            CHECKQ(mini_os_mutex_lock_isr(s_sw_mx[m]) == MINI_OS_ERR_AGAIN);
        }

#if MINI_OS_EVENT
        /* event bits accumulate while nobody waits */
        CHECKQ(mini_os_event_set_group_isr(s_sw_ev,
                                           (mini_os_uint32_t)1u << (k % 5u)) == MINI_OS_OK);
#endif

        /* the yield_isr contract, quiet side: nothing is ready at all, so the
         * call MUST return instead of switching away from the ISR */
        no_switch = 0;
        if (setjmp(g_sched) == 0)
        {
            CHECKQ(mini_os_schedule_yield_isr() == MINI_OS_OK);
            no_switch = 1;                         /* only reached without a switch */
        }
        mini_os_current_thread = MINI_OS_NULL;
        CHECKQ(no_switch == 1);
    }
}

/* the yield_isr contract, loud side: prio 0 is ready and the "interrupted"
 * thread sits at 26, so the call MUST trigger the switch (longjmp here) */
static void sw_yield_isr_switches(void)
{
    volatile int switched = 0;

    if (setjmp(g_sched) == 0)
    {
        mini_os_current_thread = s_sw_isr_host;
        (void)mini_os_schedule_yield_isr();
        switched = 1;                              /* only reached without a switch */
    }
    mini_os_current_thread = MINI_OS_NULL;
    CHECKQ(switched == 0);
}

/* --- scenario A: a three-deep PI chain, unwound by staged timeouts -------- */
static void sw_chain_low_entry(void *param)        /* prio 20, holds the outer */
{
    (void)param;
    if (s_sw_phase == 0)
    {
        s_flag = (mini_os_mutex_lock(s_sw_chain_outer, 0) == MINI_OS_OK);
    }
}

static void sw_chain_mid_entry(void *param)        /* prio 10, holds the inner */
{
    (void)param;
    if (s_sw_phase == 0)
    {
        s_flag = (mini_os_mutex_lock(s_sw_chain_inner, 0) == MINI_OS_OK);
    }
    else
    {
        (void)mini_os_mutex_lock(s_sw_chain_outer, 50);  /* parks on the low holder */
        CHECKQ(!"mid chain contender resumed mid-call");
    }
}

static void sw_chain_high_entry(void *param)       /* prio 2, parks on the mid */
{
    (void)param;
    (void)mini_os_mutex_lock(s_sw_chain_inner, 5);
    CHECKQ(!"high chain contender resumed mid-call");
}

/* --- scenario B: a triple holder dying with a waiter parked on each -------- */
static void sw_hold3_entry(void *param)            /* prio 15, parks holding 3 */
{
    (void)param;
    s_flag = (mini_os_mutex_lock(s_sw_kill3[0], 0) == MINI_OS_OK);
    s_flag &= (mini_os_mutex_lock(s_sw_kill3[1], 0) == MINI_OS_OK);
    s_flag &= (mini_os_mutex_lock(s_sw_kill3[2], 0) == MINI_OS_OK);
    (void)mini_os_thread_delay_tick(0xFFFFFFFFu);  /* park out of reach */
    CHECKQ(!"parked triple holder resumed");
}

static void sw_wait3_entry(void *param)            /* waiter, parks on one of them */
{
    (void)param;
    (void)mini_os_mutex_lock(s_sw_kill3[(mini_os_size_t)param], 100);
    CHECKQ(!"triple-kill waiter resumed mid-call");
}

static void sw_park_entry(void *param)             /* the ISR storm's interrupted thread */
{
    (void)param;
    (void)mini_os_thread_delay_tick(0xFFFFFFFFu);  /* park out of reach, forever */
    CHECKQ(!"parked ISR host resumed");
}

static void test_priority_sweep(void)
{
    mini_os_uint8_t prio = 0;
    mini_os_thread_state_t state = MINI_OS_THREAD_STATE_INVALID;
    mini_os_uint16_t cnt = 0;
    mini_os_size_t i;
    unsigned int round;

    printf("--- priority sweep: %d levels, PI chains, ISR storm ---\n", SW_TASKS);

    /* ================= scenario A: transitive PI with staged unwind ======= */
    s_sw_chain_outer = mini_os_mutex_create("sw_pi_out");
    s_sw_chain_inner = mini_os_mutex_create("sw_pi_in");
    CHECK(s_sw_chain_outer != MINI_OS_NULL && s_sw_chain_inner != MINI_OS_NULL);
    {
        mini_os_thread_t *low;
        mini_os_thread_t *mid;
        mini_os_thread_t *high;

        s_sw_phase = 0;
        s_flag = 0;
        low = mini_os_thread_create("sw_pi_low", 512, 20, sw_chain_low_entry, MINI_OS_NULL);
        CHECK(low != MINI_OS_NULL);
        run_thread_once(low);
        CHECK(s_flag == 1);                        /* low holds the outer */

        mid = mini_os_thread_create("sw_pi_mid", 512, 10, sw_chain_mid_entry, MINI_OS_NULL);
        CHECK(mid != MINI_OS_NULL);
        run_thread_once(mid);
        CHECK(s_flag == 1);                        /* mid holds the inner */
        s_sw_phase = 1;
        reset_current();
        run_thread_once(mid);                      /* parks on outer (timeout 50) */
        CHECK(mini_os_thread_get_priority(low, &prio) == MINI_OS_OK && prio == 10);

        high = mini_os_thread_create("sw_pi_high", 512, 2, sw_chain_high_entry, MINI_OS_NULL);
        CHECK(high != MINI_OS_NULL);
        reset_current();
        run_thread_once(high);                     /* parks on inner (timeout 5) */
        CHECK(mini_os_thread_get_priority(mid, &prio) == MINI_OS_OK && prio == 2);
        CHECK(mini_os_thread_get_priority(low, &prio) == MINI_OS_OK && prio == 2);

        /* deleting the parked high waiter must unwind the boost through the
         * whole chain (regression: unlink used to leave the owner boosted) */
        finish_thread(high);
        CHECK(mini_os_thread_get_priority(mid, &prio) == MINI_OS_OK && prio == 10);
        CHECK(mini_os_thread_get_priority(low, &prio) == MINI_OS_OK && prio == 10);
        finish_thread(mid);                        /* still owns inner: force-released */
        CHECK(mini_os_thread_get_priority(low, &prio) == MINI_OS_OK && prio == 20);
        CHECK(s_sw_chain_inner->owner == MINI_OS_NULL);
        finish_thread(low);
        CHECK(s_sw_chain_outer->owner == MINI_OS_NULL);
    }
    CHECK(mini_os_mutex_delete(s_sw_chain_outer) == MINI_OS_OK);
    CHECK(mini_os_mutex_delete(s_sw_chain_inner) == MINI_OS_OK);

    /* ================= scenario B: a triple holder dies ==================== */
    {
        mini_os_thread_t *victim;
        mini_os_thread_t *wa;
        mini_os_thread_t *wb;
        mini_os_thread_t *wc;

        for (i = 0; i < 3; i++)
        {
            char nm[12];

            (void)snprintf(nm, sizeof(nm), "sw_k3_%u", (unsigned)i);
            s_sw_kill3[i] = mini_os_mutex_create(nm);
            CHECK(s_sw_kill3[i] != MINI_OS_NULL);
        }
        s_flag = 0;
        victim = mini_os_thread_create("sw_hold3", 512, 15, sw_hold3_entry, MINI_OS_NULL);
        CHECK(victim != MINI_OS_NULL);
        run_thread_once(victim);                   /* parks holding all three */
        CHECK(s_flag == 1);
        CHECK(ss_hold_count(victim) == 3u);

        wa = mini_os_thread_create("sw_w3a", 512, 5, sw_wait3_entry, (void *)(mini_os_size_t)0);
        wb = mini_os_thread_create("sw_w3b", 512, 7, sw_wait3_entry, (void *)(mini_os_size_t)1);
        wc = mini_os_thread_create("sw_w3c", 512, 9, sw_wait3_entry, (void *)(mini_os_size_t)2);
        CHECK(wa != MINI_OS_NULL && wb != MINI_OS_NULL && wc != MINI_OS_NULL);
        reset_current();
        run_thread_once(wa);                       /* parks, boosts to 5 */
        reset_current();
        run_thread_once(wb);
        reset_current();
        run_thread_once(wc);
        CHECK(mini_os_thread_get_priority(victim, &prio) == MINI_OS_OK && prio == 5);
        CHECK(mini_os_thread_get_state(wa, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_BLOCKED);

        reset_current();
        s_no_yield = 0;
        if (setjmp(g_sched) == 0)
        {
            (void)mini_os_thread_delete(victim);   /* dies holding all three */
            s_no_yield = 1;
        }
        CHECK(s_no_yield == 0);                    /* waking 3 waiters yields */
        for (i = 0; i < 3; i++)
        {
            CHECK(mx_waiter_count(s_sw_kill3[i]) == 0);
            CHECK(s_sw_kill3[i]->owner == MINI_OS_NULL);
            CHECK(s_sw_kill3[i]->depth == 0u);
            CHECK(s_sw_kill3[i]->semaphore.count == s_sw_kill3[i]->semaphore.max_count);
        }
        CHECK(mini_os_thread_get_state(wa, &state) == MINI_OS_OK && state == MINI_OS_THREAD_STATE_READY);
        CHECK(wa->wait_mutex == MINI_OS_NULL && wb->wait_mutex == MINI_OS_NULL);
        finish_thread(wa);
        finish_thread(wb);
        finish_thread(wc);
        for (i = 0; i < 3; i++)
        {
            CHECK(mini_os_mutex_delete(s_sw_kill3[i]) == MINI_OS_OK);
        }
    }

    /* ================= the fleet: one task per priority 0..30 ============== */
    for (i = 0; i < SW_MX; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "sw_mx%u", (unsigned)i);
        s_sw_mx[i] = mini_os_mutex_create(nm);
        CHECK(s_sw_mx[i] != MINI_OS_NULL);
    }
    s_sw_isr_mx_a = mini_os_mutex_create("sw_isr_mx");
    CHECK(s_sw_isr_mx_a != MINI_OS_NULL);
    for (i = 0; i < SW_SEM_TOTAL; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "sw_sem%u", (unsigned)i);
        s_sw_sem[i] = mini_os_semaphore_create(nm, s_sw_sem_max[i], s_sw_sem_init[i]);
        CHECK(s_sw_sem[i] != MINI_OS_NULL);
    }
    for (i = 0; i < SW_Q_TOTAL; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "sw_q%u", (unsigned)i);
        s_sw_q[i] = mini_os_queue_create(nm, (int)sizeof(int), 32);
        CHECK(s_sw_q[i] != MINI_OS_NULL);
    }
#if MINI_OS_EVENT
    s_sw_ev = mini_os_event_group_create(0, MINI_OS_EVENT_OR_TYPE);
    CHECK(s_sw_ev != MINI_OS_NULL);
#endif

    s_sw_isr_host = mini_os_thread_create("sw_isr_host", 512, 26, sw_park_entry, MINI_OS_NULL);
    CHECK(s_sw_isr_host != MINI_OS_NULL);

    for (i = 0; i < SW_TASKS; i++)
    {
        char nm[12];

        (void)snprintf(nm, sizeof(nm), "sw_p%u", (unsigned)i);
        s_sw_t[i] = mini_os_thread_create(nm, 512, (mini_os_uint8_t)i, sw_worker_entry,
                                          (void *)(mini_os_size_t)i);
        CHECK(s_sw_t[i] != MINI_OS_NULL);
    }

    s_sw_storm_timer = mini_os_timer_create("sw_storm", sw_storm_cb, MINI_OS_NULL, 1,
                                            MINI_OS_TIMER_FLAG_PERIODIC, MINI_OS_TIMER_FLAG_HARD);
    CHECK(s_sw_storm_timer != MINI_OS_NULL);
    CHECK(mini_os_timer_start(s_sw_storm_timer) == MINI_OS_OK);

    for (round = 0; round < SW_ROUNDS; round++)
    {
        for (i = 0; i < SW_TASKS; i++)
        {
            sw_run_next();                         /* highest ready priority runs a pass */
        }
        sw_isr_storm();                            /* every fleet thread is parked here */
        drive_ticks(1);                            /* wakes the fleet, fires the HARD post */
        sw_yield_isr_switches();                   /* prio 0 is ready: the ISR must switch */
    }

    /* -------------------------- reconcile and tear down ------------------- */
    CHECK(mini_os_timer_stop(s_sw_storm_timer) == MINI_OS_OK);
    CHECK(mini_os_timer_delete(s_sw_storm_timer) == MINI_OS_OK);
    printf("  sweep: %u passes, %u storm iters, %u HARD posts, %u contested locks\n",
           (unsigned)(SW_TASKS * SW_ROUNDS), (unsigned)(SW_ROUNDS * SW_STORM_ITERS),
           s_sw_posts, s_sw_again);
    CHECK(s_sw_posts == (unsigned)SW_ROUNDS);      /* exactly one HARD post per round tick */
    CHECK(s_sw_again > 0u);                        /* cross-priority contention really happened */
    CHECK(s_sw_isr_holds_a == 0);                  /* the ISR lock/unlock cycle is balanced */

    for (i = 0; i < SW_SEM_TOTAL; i++)
    {
        unsigned int expect = (unsigned)s_sw_sem_init[i] + s_sw_gives[i] + s_sw_gisr[i] -
                              s_sw_takes[i] + ((i == SW_SEM_UNB) ? s_sw_posts : 0u);

        CHECK(mini_os_semaphore_get_count(s_sw_sem[i], &cnt) == MINI_OS_OK);
        CHECK((unsigned)cnt == expect);
        CHECK(mini_os_semaphore_delete(s_sw_sem[i]) == MINI_OS_OK);
    }
    for (i = 0; i < SW_Q_TOTAL; i++)
    {
        CHECK((unsigned)mini_os_queue_get_depth(s_sw_q[i]) ==
              s_sw_qsend[i] - s_sw_qrecv[i]);
        CHECK(mini_os_queue_delete(s_sw_q[i]) == MINI_OS_OK);
    }
#if MINI_OS_EVENT
    {
        mini_os_uint32_t bits = 0;

        CHECK(mini_os_event_get_group(s_sw_ev, &bits) == MINI_OS_OK);
        CHECK(bits == 0x1Fu);                      /* every ISR-set bit survived (no waiter) */
        CHECK(mini_os_event_group_delete(s_sw_ev) == MINI_OS_OK);
    }
#endif
    for (i = 0; i < SW_MX; i++)
    {
        CHECK(s_sw_mx[i]->owner == MINI_OS_NULL);
        CHECK(mini_os_mutex_delete(s_sw_mx[i]) == MINI_OS_OK);
    }
    CHECK(s_sw_isr_mx_a->owner == MINI_OS_NULL);
    CHECK(mini_os_mutex_delete(s_sw_isr_mx_a) == MINI_OS_OK);

    for (i = 0; i < SW_TASKS; i++)
    {
        CHECK(s_sw_step[i] == SW_ROUNDS);          /* every priority ran every round */
        CHECK(ss_hold_count(s_sw_t[i]) == 0u);     /* nothing leaked from any level */
        CHECK(mini_os_thread_get_state(s_sw_t[i], &state) == MINI_OS_OK &&
              state == MINI_OS_THREAD_STATE_READY);
        finish_thread(s_sw_t[i]);
    }
    finish_thread(s_sw_isr_host);                  /* parked forever: delete unlinks it */
}

/* ================================= main ==================================== */
int main(void)
{
    printf("mini-os full-kernel host test (all features on)\n\n");

    CHECK(mini_os_heap_free_space() > 0);           /* heap constructor ran before main */
    CHECK(mini_os_schedule_init() == MINI_OS_OK);
    CHECK(mini_os_schedule_init() == MINI_OS_OK);   /* re-init is accepted while empty */

    test_thread_basics();
    test_schedule();
    test_semaphore();
    test_mutex();
    test_queue();
#if MINI_OS_EVENT
    test_event();
#endif
    test_timer_hard();
    test_timer_soft();
    test_wheel_delay();
#if MINI_OS_THREAD_DETACH
    test_join_reap();
#endif
#if MINI_OS_FIND_BY_NAME
    test_find_by_name();
#endif
    test_super_stress();
    test_super_timers();
    test_priority_sweep();

    printf("\n%d checks, %d failures\n", checks, failures);
    return (failures == 0) ? 0 : 1;
}
