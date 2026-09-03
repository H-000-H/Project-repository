/*
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file test_mem.c
 * @brief Host test for the malloc/free model allocator (memory.h / memory.c)
 *
 * Build & run (host, no ARM toolchain needed):
 *   clang -std=c11 -Wall -Wextra -Itest -Iinc -include redef.h test/test_mem.c src/memory.c \
 *         -o build/test_mem.exe && build/test_mem.exe
 *
 * With slab (small-allocation path, note: the knob is CONFIG_MINI_OS_SLAB_PAGE_SIZE,
 * as read by inc/mem_heap.h — a stale CONFIG_MINI_MALLOC_SLAB_PAGE_SIZE here
 * would silently leave the 2 KB default page and the 1 KB pools cannot even
 * carve one page, so init fails and later cases run on an uninitialized pool):
 *   clang -std=c11 -Wall -Wextra -DCONFIG_OPEN_SLAB -DCONFIG_MINI_OS_SLAB_PAGE_SIZE=256 \
 *         -Itest -Iinc -include redef.h test/test_mem.c src/memory.c \
 *         -o build/test_mem_slab.exe && build/test_mem_slab.exe
 *
 */
#include "memory.h"
#include "err.h"
#if defined(CONFIG_OPEN_SLAB) || defined(CONFIG_MINI_OS_SLAB_STATIC)
#include "mem_heap.h" /* MINI_OS_SLAB_PAGE_SIZE / MINI_OS_SLAB_STATIC_SIZE 等 slab 配置宏 */
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
    } while (0)

/* slab 构建下 init 会从池首划走固定区, 池空闲上限 = 总量 - slab 区 */
#ifdef CONFIG_OPEN_SLAB
#define TEST_FREE_CAP(p, total) ((total) - (p).slab_size)
#else
#define TEST_FREE_CAP(p, total) (total)
#endif

/* 多档槽区: 段 i -> 档位 (i %% 档位数), 档位尺寸 = 16 << 档位序号 */
#define TEST_SLAB_SEG_SLOT_SIZE(seg) ((mini_os_size_t)MINI_OS_SLAB_MINI_BYTES << ((seg) % MINI_OS_SLAB_CLASS_COUNT))
#define TEST_SLAB_SEG_SLOTS(seg) (MINI_OS_SLAB_PAGE_SIZE / TEST_SLAB_SEG_SLOT_SIZE(seg))

/* 档位测试池: 8*PAGE_SIZE = /比例 后恰划 2 页 (段 0 -> 16B, 段 1 -> 32B) */
#define TEST_SLAB_POOL_SIZE (8u * MINI_OS_SLAB_PAGE_SIZE)

/* 全局堆划出的页数 = min(PAGE_MAX, 堆/比例/页大小), 与 init 公式一致 */
#define TEST_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TEST_GLOBAL_SLAB_PAGES TEST_MIN((mini_os_size_t)MINI_OS_SLAB_PAGE_MAX, ((mini_os_size_t)sizeof(__mini_os_heap_start) / MINI_OS_SLAB_PROPORTION) / MINI_OS_SLAB_PAGE_SIZE)

/* Linker heap symbol stubs: 宿主构建没有链接脚本, 用一段静态缓冲模拟堆区:
 * start 为缓冲首地址, end 紧随其后, MINI_OS_HEAP_SIZE 即缓冲大小。
 * 启动构造器 (优先级 MINI_OS_MEMORY_PRESTRUCTOR) 会在 main 前接管它。
 * 大小取 16KB: 512 档构建 (页 512B) 下全局堆仍能划出 PAGE_MAX=6 页含 512B 档 */
char __mini_os_heap_start[16384] __attribute__((aligned(8)));
char __mini_os_heap_end[1];

_Alignas(8) static mini_os_uint8_t seg2_mem[700];
_Alignas(8) static mini_os_uint8_t seg3_mem[256];
_Alignas(8) static mini_os_uint8_t seg4_mem[256];
#ifdef CONFIG_MINI_OS_SLAB_512
/* 页 512B: 普通池测试也需要能划出页, 取 4*页大小*2 = 划 2 页 */
_Alignas(8) static mini_os_uint8_t pool_mem[4096];
#define TEST_ALLOC_EXCEED 5000u /* 超过 4096 池 (含划页后剩余 3072) */
#else
_Alignas(8) static mini_os_uint8_t pool_mem[1024];
#define TEST_ALLOC_EXCEED 1008u /* 1008+块头 > 1024 池 */
#endif
#ifdef CONFIG_OPEN_SLAB
_Alignas(8) static mini_os_uint8_t slab_pool_mem[TEST_SLAB_POOL_SIZE];
#endif

/* ---------------------------------------------------------------------- */
static void test_global_heap(void)
{
    void* p;
    void* q;

    printf("--- global heap (host stub region) ---\n");
    /* 构造器已在 main 前接管桩堆区, 分配应当可用 */
    p = mini_os_malloc(32u);
    CHECK(p != MINI_OS_NULL);
    if (p != MINI_OS_NULL)
    {
        memset(p, 0x5Au, 32u);
        CHECK(((mini_os_uint8_t*)p)[31] == 0x5Au);
    }
    q = mini_os_malloc(64u);
    CHECK(q != MINI_OS_NULL);
    CHECK(q != p);
    CHECK(mini_os_free(p) == MINI_OS_OK);
    CHECK(mini_os_free(q) == MINI_OS_OK);
    CHECK(mini_os_free(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
}

/* ---------------------------------------------------------------------- */
static void test_calloc(void)
{
    mini_os_uint8_t* p;
    mini_os_size_t i;

    printf("--- calloc (global heap) ---\n");
    p = mini_os_calloc(4u, 64u); /* 256 字节清零 */
    CHECK(p != MINI_OS_NULL);
    if (p != MINI_OS_NULL)
    {
        for (i = 0u; i < 256u; i++)
        {
            CHECK(p[i] == 0u);
        }
        p[255] = 0xABu;
        CHECK(mini_os_free(p) == MINI_OS_OK);
    }
    CHECK(mini_os_calloc(0u, 16u) == MINI_OS_NULL); /* 0 尺寸跟随 malloc 语义 */
    CHECK(mini_os_calloc(4u, 0u) == MINI_OS_NULL);
    CHECK(mini_os_calloc((mini_os_size_t)-1, 16u) == MINI_OS_NULL); /* 乘法溢出拒绝 */
}

/* ---------------------------------------------------------------------- */
static void test_init_validation(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "test", pool_mem, sizeof(pool_mem) };

    printf("--- init validation ---\n");
    CHECK(mini_os_memory_init(MINI_OS_NULL, &cfg) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_memory_init(&pool, MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    {
        mini_os_memory_config_t null_mem_cfg = { "bad", MINI_OS_NULL, sizeof(pool_mem) };
        CHECK(mini_os_memory_init(&pool, &null_mem_cfg) == MINI_OS_ERR_INVAL);
    }
    {
        mini_os_memory_config_t tiny_cfg = { "tiny", pool_mem, 32 };
        CHECK(mini_os_memory_init(&pool, &tiny_cfg) == MINI_OS_ERR_INVAL);
    }

    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    CHECK(mini_os_memory_size(&pool) == sizeof(pool_mem));
    CHECK(mini_os_memory_used(&pool) == 0u);
    CHECK(mini_os_memory_peak(&pool) == 0u);
    CHECK(mini_os_memory_free_space(&pool) == TEST_FREE_CAP(pool, sizeof(pool_mem)));
    CHECK(strcmp(pool.name, "test") == 0);
    CHECK(mini_os_memory_deinit(&pool) == MINI_OS_OK);
}

/* ---------------------------------------------------------------------- */
static void test_alloc_free_cycle(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "cycle", pool_mem, sizeof(pool_mem) };
    void* a;
    void* b;

    printf("--- alloc/free cycle ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);

    a = mini_os_memory_alloc(&pool, 64u);
    CHECK(a != MINI_OS_NULL);
    if (a != MINI_OS_NULL)
    {
        CHECK((((mini_os_size_t)a) % 8u) == 0u); /* malloc 语义: 8 对齐裸指针 */
        CHECK((mini_os_uint8_t*)a >= pool_mem);
        CHECK((mini_os_uint8_t*)a + 64u <= pool_mem + sizeof(pool_mem));
        CHECK(mini_os_memory_used(&pool) == 1u);
        CHECK(mini_os_memory_peak(&pool) == 1u);
    }

    b = mini_os_memory_alloc(&pool, 64u);
    CHECK(b != MINI_OS_NULL);
    if (b != MINI_OS_NULL)
    {
        CHECK(b != a);
        CHECK(mini_os_memory_used(&pool) == 2u);
        CHECK(mini_os_memory_peak(&pool) == 2u);
        CHECK(mini_os_memory_free_space(&pool) < sizeof(pool_mem));
    }

    CHECK(mini_os_memory_free(&pool, a) == MINI_OS_OK);
    CHECK(mini_os_memory_used(&pool) == 1u);
    CHECK(mini_os_memory_free(&pool, b) == MINI_OS_OK);
    CHECK(mini_os_memory_used(&pool) == 0u);
    /* 两块地址相邻: 释放后合并回整池单块 */
    CHECK(mini_os_memory_free_space(&pool) == TEST_FREE_CAP(pool, sizeof(pool_mem)));
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_data_roundtrip(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "data", pool_mem, sizeof(pool_mem) };
    void* blk;
    mini_os_uint8_t pattern[64];
    mini_os_size_t i;

    printf("--- data roundtrip (no FIFO, caller owns bytes) ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    blk = mini_os_memory_alloc(&pool, 64u);
    CHECK(blk != MINI_OS_NULL);
    if (blk == MINI_OS_NULL)
    {
        return;
    }
    /* 直接按裸指针读写, 无块内指针管理 */
    for (i = 0u; i < 64u; i++)
    {
        pattern[i] = (mini_os_uint8_t)(0xA0u + i);
    }
    memcpy(blk, pattern, sizeof(pattern));
    CHECK(memcmp(blk, pattern, sizeof(pattern)) == 0);
    memset(blk, 0x5Au, 64u);
    CHECK(((mini_os_uint8_t*)blk)[0] == 0x5Au);
    CHECK(((mini_os_uint8_t*)blk)[63] == 0x5Au);

    mini_os_memory_free(&pool, blk);
    CHECK(mini_os_memory_used(&pool) == 0u);

    /* 释放后重新分配: 同地址复用 (整池唯一空闲块) */
    blk = mini_os_memory_alloc(&pool, 32u);
    CHECK(blk != MINI_OS_NULL);
    mini_os_memory_free(&pool, blk);
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_peak_and_stats(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "peak", pool_mem, sizeof(pool_mem) };
    void* a;
    void* b;

    printf("--- peak / used tracking ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    a = mini_os_memory_alloc(&pool, 32u);
    b = mini_os_memory_alloc(&pool, 32u);
    CHECK(a != MINI_OS_NULL && b != MINI_OS_NULL);
    CHECK(mini_os_memory_used(&pool) == 2u);
    CHECK(mini_os_memory_peak(&pool) == 2u);

    mini_os_memory_free(&pool, a);
    CHECK(mini_os_memory_used(&pool) == 1u);
    CHECK(mini_os_memory_peak(&pool) == 2u); /* 峰值不随释放下降 */

    mini_os_memory_reset_peak(&pool);
    CHECK(mini_os_memory_peak(&pool) == 1u);

    a = mini_os_memory_alloc(&pool, 32u);
    CHECK(a != MINI_OS_NULL);
    CHECK(mini_os_memory_peak(&pool) == 2u); /* 重新攀升 */

    mini_os_memory_free(&pool, a);
    mini_os_memory_free(&pool, b);
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_isr_variants(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "isr", pool_mem, sizeof(pool_mem) };
    void* blk;

    printf("--- ISR variants ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    blk = mini_os_memory_alloc_isr(&pool, 64u);
    CHECK(blk != MINI_OS_NULL);
    CHECK(mini_os_memory_used(&pool) == 1u);
    mini_os_memory_free_isr(&pool, blk);
    CHECK(mini_os_memory_used(&pool) == 0u);
    CHECK(mini_os_memory_free_space(&pool) == TEST_FREE_CAP(pool, sizeof(pool_mem)));
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_exhaustion(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "exh", pool_mem, sizeof(pool_mem) };
    void* blocks[32];
    mini_os_size_t count = 0u;
    mini_os_size_t i;
    mini_os_size_t j;

    printf("--- exhaustion ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    while (count < 32u)
    {
        void* blk = mini_os_memory_alloc(&pool, 200u);
        if (blk == MINI_OS_NULL)
        {
            break;
        }
        for (j = 0u; j < count; j++)
        {
            CHECK(blocks[j] != blk); /* 无重叠分配 */
        }
        blocks[count++] = blk;
    }
    CHECK(count >= (mini_os_size_t)(TEST_FREE_CAP(pool, sizeof(pool_mem)) / (200u + sizeof(mini_os_buffer_freelist_config_t)))); /* 空闲区/每块(200+块头) */
    CHECK(mini_os_memory_used(&pool) == count);
    CHECK(mini_os_memory_peak(&pool) == count);
    CHECK(mini_os_memory_alloc(&pool, 200u) == MINI_OS_NULL); /* 真正耗尽 */

    for (i = 0u; i < count; i++)
    {
        mini_os_memory_free(&pool, blocks[i]);
    }
    CHECK(mini_os_memory_used(&pool) == 0u);
    CHECK(mini_os_memory_free_space(&pool) == TEST_FREE_CAP(pool, sizeof(pool_mem)));
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_expand(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "expand", pool_mem, sizeof(pool_mem) };
    void* blk;

    printf("--- expand ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    CHECK(mini_os_memory_alloc(&pool, TEST_ALLOC_EXCEED) == MINI_OS_NULL); /* 超池剩余容量, 拒绝 */
    CHECK(mini_os_memory_expand(&pool, seg2_mem, sizeof(seg2_mem)) == MINI_OS_OK);
    CHECK(mini_os_memory_size(&pool) == sizeof(pool_mem) + sizeof(seg2_mem));

    blk = mini_os_memory_alloc(&pool, 600u);
    CHECK(blk != MINI_OS_NULL);
    if (blk != MINI_OS_NULL)
    {
        memset(blk, 0x5Au, 600u);
        CHECK(((mini_os_uint8_t*)blk)[599] == 0x5Au);
        mini_os_memory_free(&pool, blk);
    }

    /* 段表满: 再加两段后拒绝 */
    CHECK(mini_os_memory_expand(&pool, seg3_mem, sizeof(seg3_mem)) == MINI_OS_OK);
    CHECK(mini_os_memory_expand(&pool, seg4_mem, sizeof(seg4_mem)) == MINI_OS_OK);
    CHECK(mini_os_memory_expand(&pool, seg2_mem, sizeof(seg2_mem)) == MINI_OS_ERR_NOSPC);

    /* 非法扩容入参 */
    CHECK(mini_os_memory_expand(&pool, MINI_OS_NULL, 256u) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_memory_expand(&pool, seg2_mem, 32u) == MINI_OS_ERR_INVAL);
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
static void test_invalid_args(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "args", pool_mem, sizeof(pool_mem) };

    printf("--- invalid args ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);

    CHECK(mini_os_memory_alloc(MINI_OS_NULL, 32u) == MINI_OS_NULL);
    CHECK(mini_os_memory_alloc(&pool, 0u) == MINI_OS_NULL);
    CHECK(mini_os_memory_free(MINI_OS_NULL, MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_memory_free(&pool, MINI_OS_NULL) == MINI_OS_ERR_INVAL);

    CHECK(mini_os_memory_size(MINI_OS_NULL) == 0u);
    CHECK(mini_os_memory_free_space(MINI_OS_NULL) == 0u);
    CHECK(mini_os_memory_used(MINI_OS_NULL) == 0u);
    CHECK(mini_os_memory_peak(MINI_OS_NULL) == 0u);
    CHECK(mini_os_memory_reset_peak(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_memory_deinit(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    CHECK(mini_os_memory_deinit(&pool) == MINI_OS_OK);
}

/* ---------------------------------------------------------------------- */
static void test_deinit_reinit(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "reinit", pool_mem, sizeof(pool_mem) };
    void* blk;

    printf("--- deinit / reinit ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    blk = mini_os_memory_alloc(&pool, 64u);
    CHECK(blk != MINI_OS_NULL);
    mini_os_memory_deinit(&pool);
    CHECK(mini_os_memory_size(&pool) == 0u);
    CHECK(mini_os_memory_used(&pool) == 0u);
    CHECK(mini_os_memory_free_space(&pool) == 0u);

    /* 换名重新初始化; 旧指针已失效 */
    {
        mini_os_memory_config_t renamed = { "R", pool_mem, sizeof(pool_mem) };
        CHECK(mini_os_memory_init(&pool, &renamed) == MINI_OS_OK);
        CHECK(strcmp(pool.name, "R") == 0);
    }
    CHECK(mini_os_memory_free_space(&pool) == TEST_FREE_CAP(pool, sizeof(pool_mem)));
    blk = mini_os_memory_alloc(&pool, 128u);
    CHECK(blk != MINI_OS_NULL);
    mini_os_memory_free(&pool, blk);
    mini_os_memory_deinit(&pool);
}

/* ---------------------------------------------------------------------- */
#ifdef CONFIG_OPEN_SLAB
static void test_slab(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "slab", slab_pool_mem, sizeof(slab_pool_mem) };
    void* c16[TEST_SLAB_SEG_SLOTS(0)];
    void* c32[TEST_SLAB_SEG_SLOTS(1)];
    void* fallback1;
    void* fallback2;
    void* reused;
    void* p;
    mini_os_size_t base_free;
    mini_os_size_t i;

    printf("--- slab size classes (CONFIG_OPEN_SLAB) ---\n");
    /* 池小于 页大小*比例: 配置了 slab 却划不出 1 页, init 直接失败 */
    {
        _Alignas(8) static mini_os_uint8_t tiny_mem[512];
        mini_os_memory_config_t tiny_cfg = { "tiny", tiny_mem, sizeof(tiny_mem) };

        CHECK(mini_os_memory_init(&pool, &tiny_cfg) == MINI_OS_ERR_NOMEM);
    }
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    /* 页数 = min(PAGE_MAX, 池/比例/页大小) = 2: 段 0 -> 16B 档, 段 1 -> 32B 档 */
    CHECK(pool.slab_page_count == 2u);
    CHECK(pool.slab_size == 2u * MINI_OS_SLAB_PAGE_SIZE);
    base_free = mini_os_memory_free_space(&pool);
    CHECK(base_free == sizeof(slab_pool_mem) - pool.slab_size);

    /* 17B 向上取整走 32B 档 (段 1): 池空闲不变 */
    fallback1 = mini_os_memory_alloc(&pool, 17u);
    CHECK(fallback1 != MINI_OS_NULL);
    CHECK(mini_os_memory_free_space(&pool) == base_free);
    CHECK(mini_os_memory_used(&pool) == 1u);
    CHECK(mini_os_memory_free(&pool, fallback1) == MINI_OS_OK);
    CHECK(mini_os_memory_used(&pool) == 0u);

    /* 16B 档 (段 0) 填满: 池空闲不变 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(0); i++)
    {
        c16[i] = mini_os_memory_alloc(&pool, MINI_OS_SLAB_MINI_BYTES);
        CHECK(c16[i] != MINI_OS_NULL);
    }
    CHECK(mini_os_memory_free_space(&pool) == base_free);
    /* 档尽: 第 17 个 16B 回退 freelist */
    fallback1 = mini_os_memory_alloc(&pool, MINI_OS_SLAB_MINI_BYTES);
    CHECK(fallback1 != MINI_OS_NULL);
    CHECK(mini_os_memory_free_space(&pool) == base_free - MINI_OS_SLAB_MINI_BYTES - sizeof(mini_os_buffer_freelist_config_t));

    /* 32B 档 (段 1) 填满: 池空闲不变 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(1); i++)
    {
        c32[i] = mini_os_memory_alloc(&pool, MINI_OS_SLAB_MINI_BYTES * 2u);
        CHECK(c32[i] != MINI_OS_NULL);
    }
    CHECK(mini_os_memory_free_space(&pool) == base_free - MINI_OS_SLAB_MINI_BYTES - sizeof(mini_os_buffer_freelist_config_t));
    /* 档尽: 第 9 个 32B 回退 freelist (累计两块回退: 16B + 32B, 各带块头) */
    fallback2 = mini_os_memory_alloc(&pool, MINI_OS_SLAB_MINI_BYTES * 2u);
    CHECK(fallback2 != MINI_OS_NULL);
    CHECK(mini_os_memory_free_space(&pool) == base_free - MINI_OS_SLAB_MINI_BYTES * 3u - sizeof(mini_os_buffer_freelist_config_t) * 2u);

    /* 33B 需 64B 档, 池只有 2 段 (16/32) -> 直接回退 freelist */
    reused = mini_os_memory_alloc(&pool, 33u);
    CHECK(reused != MINI_OS_NULL);

    /* LIFO 复用: 释放一个 16B 槽后立即复用 */
    CHECK(mini_os_memory_free(&pool, c16[7]) == MINI_OS_OK);
    p = mini_os_memory_alloc(&pool, MINI_OS_SLAB_MINI_BYTES);
    CHECK(p == c16[7]);
    c16[7] = p;

#ifdef CONFIG_MINI_OS_SLAB_512
    /* 512B 档 (全局堆划出 PAGE_MAX 页全档): 走堆内 slab, 堆空闲不变 */
    p = mini_os_malloc(512u);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start) - (TEST_GLOBAL_SLAB_PAGES * MINI_OS_SLAB_PAGE_SIZE));
    CHECK(mini_os_free(p) == MINI_OS_OK);
#endif

    /* 全部归还: 空闲回到 init 后水平 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(0); i++)
    {
        CHECK(mini_os_memory_free(&pool, c16[i]) == MINI_OS_OK);
    }
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(1); i++)
    {
        CHECK(mini_os_memory_free(&pool, c32[i]) == MINI_OS_OK);
    }
    CHECK(mini_os_memory_free(&pool, fallback1) == MINI_OS_OK);
    CHECK(mini_os_memory_free(&pool, fallback2) == MINI_OS_OK);
    CHECK(mini_os_memory_free(&pool, reused) == MINI_OS_OK);
    CHECK(mini_os_memory_used(&pool) == 0u);
    CHECK(mini_os_memory_free_space(&pool) == base_free);
    mini_os_memory_deinit(&pool);
}
#endif /* CONFIG_OPEN_SLAB */

/* ---------------------------------------------------------------------- */
#ifdef CONFIG_MINI_OS_SLAB_STATIC
/* 静态版多档槽区: 独立静态数组, 不从堆划出, 档尽回退堆;
 * 只作用于全局入口, 池 API 不受影响 */
static void test_slab_static(void)
{
    mini_os_memory_t pool;
    mini_os_memory_config_t cfg = { "static", pool_mem, sizeof(pool_mem) };
    void* c16[TEST_SLAB_SEG_SLOTS(0)];
    void* c32[TEST_SLAB_SEG_SLOTS(1)];
    void* p;
    mini_os_size_t i;

    printf("--- static slab zone (CONFIG_MINI_OS_SLAB_STATIC) ---\n");
    CHECK(mini_os_memory_init(&pool, &cfg) == MINI_OS_OK);
    /* 静态版不从堆划出: 池空闲容量 = 总量, 全局堆空闲 = 全部堆区 */
    CHECK(mini_os_memory_free_space(&pool) == sizeof(pool_mem));
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));

    /* 8B 走 16B 档 (段 0): 池与堆空闲均不变 */
    c16[0] = mini_os_malloc(8u);
    CHECK(c16[0] != MINI_OS_NULL);
    CHECK(mini_os_memory_free_space(&pool) == sizeof(pool_mem));
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));
    CHECK(mini_os_free(c16[0]) == MINI_OS_OK);

    /* 17B 向上取整走 32B 档 (段 1): 堆空闲不变 */
    p = mini_os_malloc(17u);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));
    CHECK(mini_os_free(p) == MINI_OS_OK);

    /* 16B 档 (段 0) 填满: 堆空闲不变 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(0); i++)
    {
        c16[i] = mini_os_malloc(MINI_OS_SLAB_MINI_BYTES);
        CHECK(c16[i] != MINI_OS_NULL);
    }
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));
    /* 档尽: 第 17 个 16B 回退堆 */
    p = mini_os_malloc(MINI_OS_SLAB_MINI_BYTES);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() < sizeof(__mini_os_heap_start));
    CHECK(mini_os_free(p) == MINI_OS_OK);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));

    /* 32B 档 (段 1) 填满: 堆空闲不变 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(1); i++)
    {
        c32[i] = mini_os_malloc(MINI_OS_SLAB_MINI_BYTES * 2u);
        CHECK(c32[i] != MINI_OS_NULL);
    }
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));

#ifndef CONFIG_MINI_OS_SLAB_512
    /* 33B 需 64B 档, 静态区只有 2 段 (16/32) -> 回退堆 (33 对齐到 40) */
    p = mini_os_malloc(33u);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start) - 40u - sizeof(mini_os_buffer_freelist_config_t));
#else
    /* 33B 需 64B 档, 静态区 3 段含 64B 档 -> 走静态区, 堆空闲不变 */
    p = mini_os_malloc(33u);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));
#endif
    CHECK(mini_os_free(p) == MINI_OS_OK);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));

#ifdef CONFIG_MINI_OS_SLAB_512
    /* 512B 档 (静态区 >=6 段): 走静态区, 堆空闲不变 */
    p = mini_os_malloc(512u);
    CHECK(p != MINI_OS_NULL);
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));
    CHECK(mini_os_free(p) == MINI_OS_OK);
#endif

    /* LIFO 复用: 释放一个 16B 槽后立即复用 */
    CHECK(mini_os_free(c16[7]) == MINI_OS_OK);
    p = mini_os_malloc(MINI_OS_SLAB_MINI_BYTES);
    CHECK(p == c16[7]);
    c16[7] = p;

    /* 全部归还: 全局堆完全恢复 */
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(0); i++)
    {
        CHECK(mini_os_free(c16[i]) == MINI_OS_OK);
    }
    for (i = 0u; i < TEST_SLAB_SEG_SLOTS(1); i++)
    {
        CHECK(mini_os_free(c32[i]) == MINI_OS_OK);
    }
    CHECK(mini_os_heap_free_space() == sizeof(__mini_os_heap_start));

    /* 数据往返: 静态区分配的块可正常读写 */
    p = mini_os_malloc(MINI_OS_SLAB_MINI_BYTES);
    CHECK(p != MINI_OS_NULL);
    MINI_OS_MEMSET(p, 0xA5, MINI_OS_SLAB_MINI_BYTES);
    CHECK(((mini_os_uint8_t*)p)[MINI_OS_SLAB_MINI_BYTES - 1u] == 0xA5u);
    CHECK(mini_os_free(p) == MINI_OS_OK);

    /* 非法指针拒绝 */
    CHECK(mini_os_free(MINI_OS_NULL) == MINI_OS_ERR_INVAL);
    mini_os_memory_deinit(&pool);
}
#endif /* CONFIG_MINI_OS_SLAB_STATIC */

/* ---------------------------------------------------------------------- */
int main(void)
{
    printf("mini-os memory (malloc/free model) host test\n");
#ifdef CONFIG_OPEN_SLAB
    printf("CONFIG_OPEN_SLAB on: page=%u max_pages=%u proportion=%u\n",
           (unsigned)MINI_OS_SLAB_PAGE_SIZE, (unsigned)MINI_OS_SLAB_PAGE_MAX, (unsigned)MINI_OS_SLAB_PROPORTION);
#endif

    test_global_heap();
    test_calloc();
    test_init_validation();
    test_alloc_free_cycle();
    test_data_roundtrip();
    test_peak_and_stats();
    test_isr_variants();
    test_exhaustion();
    test_expand();
    test_invalid_args();
    test_deinit_reinit();
#ifdef CONFIG_OPEN_SLAB
    test_slab();
#endif
#ifdef CONFIG_MINI_OS_SLAB_STATIC
    test_slab_static();
#endif

    printf("----------------------------------------------\n");
    printf("%d checks, %d failure(s)\n", checks, failures);
    if (failures == 0)
    {
        printf("ALL TESTS PASSED\n");
    }
    return failures == 0 ? 0 : 1;
}
