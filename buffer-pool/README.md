# buffer_pool

A standalone, C-standard-library-only memory pool: a **free-list (doubly linked) + dual-pointer block allocator** for embedded systems. It is intentionally decoupled from any framework header — copy `buffer_pool.h` + `buffer_pool.c` into your project and compile.

## Why

Dynamic `malloc`/`free` fragments memory and is often not allowed in MCU critical paths. `buffer_pool` cuts caller-sized blocks from a pre-allocated static pool with **first-fit** allocation and **automatic coalescing** on free, replacing scattered `malloc` in DMA buffers, driver I/O queues, event passing, etc.

## Design

- **Free list (doubly linked, prev/next)**: manages *how much is cut from the pool*. The static pool is split into variable-size blocks; block metadata is embedded in the header, and adjacent blocks are coalesced back together on free. Blocks are 8-byte aligned; the remainder is kept as a new free block when it is big enough.
- **Dual pointers (head/tail)**: manage *how the block content is used*. Each allocated block is a ring read/write buffer — `head` advances on write, `tail` on read, with wraparound. Writes beyond capacity are truncated.
- **Atomic, wrapped internally** (`BUFF_POOL_*` macros): on ARMv6-M / ARMv8-M Baseline the critical section is implemented by masking interrupts; everywhere else GCC `__atomic` builtins are used. No framework atomics, no spinlock dependency.
- **Two modes** (compile-time, see below): static-first (falls back to dynamic when the pool is exhausted) or dynamic-only (the static pool is disabled entirely).
- **Gate macro `CONFIG_BUFFER_POOL`**: when integrating into a Kconfig-based build (e.g. mini_tree), define it (`=y`) to compile the file; when using standalone, simply leave it undefined (a default `CONFIG_BUFFER_POOL_SIZE` of 4096 is provided as a fallback).

## Files

| File | Description |
|:---|:---|
| `buffer_pool.h` | Public API (C, C++-safe) |
| `buffer_pool.c` | Implementation (C99, no framework headers) |

## Compile-time configuration

| Macro | Default | Meaning |
|:---|:---|:---|
| `CONFIG_BUFFER_POOL` | undefined | Gate: only compile the implementation when defined |
| `CONFIG_BUFFER_POOL_SIZE` | `4096` | Default static pool size in bytes when the caller does not supply `static_mem` |
| `CONFIG_BUFFER_POOL_DYNAMIC_ONLY` | undefined | When defined, the static pool is disabled and all allocations go to dynamic memory |

Modes:
- **Static-first (default)**: the free list cuts blocks from the static pool; when exhausted, allocation falls back to dynamic (`calloc`/`free`).
- **Dynamic-only**: the static pool is masked; every allocation goes straight to dynamic memory (requires a working heap).

## Usage

### 1. Add the files to your project

Copy `buffer_pool.h` and `buffer_pool.c` into your source tree (e.g. `components/buffer_pool/` or `lib/buffer_pool/`), add the directory to your include path and compile `buffer_pool.c` with your C99+ toolchain. That is all — no framework headers, no extra dependencies.

### 2. Choose a mode (compile-time)

| Mode | How to enable | Behavior |
|:---|:---|:---|
| **Static-first** (default) | do not define anything | free list cuts blocks from the static pool; falls back to `calloc` when exhausted |
| **Dynamic-only** | `-DCONFIG_BUFFER_POOL_DYNAMIC_ONLY` | the static pool is masked; every allocation goes to dynamic memory |

Optional: `-DCONFIG_BUFFER_POOL_SIZE=8192` to change the default static pool size (used when the caller passes `static_len == 0`). When integrated into a Kconfig build, define `CONFIG_BUFFER_POOL` so the implementation compiles; standalone use can leave it undefined (the file self-gates via `#ifdef`).

### 3. Typical lifecycle

```c
/* 3.1 Create a pool (static pool backed by your own memory) */
static uint8_t s_pool_mem[4096] _Alignas(8);
buffer_pool_t* pool = buffer_pool_create(&(buffer_pool_config_t){
    .name       = "net",
    .use_static = true,
    .static_mem = s_pool_mem,
    .static_len = sizeof(s_pool_mem),
});
if (pool == NULL) { /* out of memory or invalid config */ }

/* 3.2 Allocate a block (caller picks the size — not fixed) */
buffer_block_t* blk = buffer_pool_alloc(pool, 1024);
if (blk == NULL) { /* pool exhausted AND heap allocation failed */ }

/* 3.3 Write / read through the dual pointers */
size_t written = 0, read = 0;
buffer_block_write(blk, tx_data, tx_len, &written);   /* written = actual bytes */
buffer_block_read(blk, rx_buf, sizeof(rx_buf), &read); /* read     = actual bytes */

/* 3.4 Return the block, then destroy the pool when done */
buffer_pool_free(pool, blk);
buffer_pool_destroy(pool);
```

> **Return convention**: `buffer_block_write`/`read` return `0` on success or `-EINVAL` on invalid arguments. The actual byte count is reported through the `actual` out-parameter (may be `NULL`).

### 4. Multiple pools

Each `buffer_pool_create()` returns an independent pool (own lock, own static memory, own free list, own counters). Create as many as you need — one per subsystem.

## Complete examples

### Example 1 — producer/consumer ring buffer (single pool)

```c
#include "buffer_pool.h"
#include <stdio.h>
#include <string.h>   /* memcmp */

static uint8_t s_pool_mem[2048] _Alignas(8);
static buffer_pool_t* s_pool;

static int setup_pool(void)
{
    s_pool = buffer_pool_create(&(buffer_pool_config_t){
        .name = "uart",
        .use_static = true,
        .static_mem = s_pool_mem,
        .static_len = sizeof(s_pool_mem),
    });
    return (s_pool != NULL) ? 0 : -1;
}

/* Producer: allocate a block, write a frame, hand it over (or free it) */
static int produce(const uint8_t* data, size_t len)
{
    buffer_block_t* blk = buffer_pool_alloc(s_pool, len);
    size_t n = 0;
    if (blk == NULL)
        return -1;
    buffer_block_write(blk, data, len, &n);
    /* Read the same block back through the dual pointers (self-check),
     * then return the block to the pool. */
    uint8_t buf[8];
    size_t r = 0;
    buffer_block_read(blk, buf, sizeof(buf), &r);
    if (r != n || memcmp(buf, data, n) != 0)
        return -1;
    buffer_pool_free(s_pool, blk);
    return 0;
}

int main(void)
{
    uint8_t frame[] = {0x01, 0x02, 0x03, 0x04};
    if (setup_pool() != 0)
        return 1;

    if (produce(frame, sizeof(frame)) != 0)
        return 2;

    printf("pool size=%zu free=%zu used=%u peak=%u\n",
           buffer_pool_size(s_pool),
           buffer_pool_free_space(s_pool),
           buffer_pool_used(s_pool),
           buffer_pool_peak(s_pool));

    buffer_pool_destroy(s_pool);
    return 0;
}
```

### Example 2 — multiple isolated pools (net + DMA)

```c
static uint8_t s_net_mem[4096] _Alignas(8);
static uint8_t s_dma_mem[1024] _Alignas(32);   /* DMA needs 32-byte alignment */

buffer_pool_t* g_net_pool;
buffer_pool_t* g_dma_pool;

int pools_init(void)
{
    g_net_pool = buffer_pool_create(&(buffer_pool_config_t){
        .name = "net", .use_static = true,
        .static_mem = s_net_mem, .static_len = sizeof(s_net_mem),
    });
    g_dma_pool = buffer_pool_create(&(buffer_pool_config_t){
        .name = "dma", .use_static = true,
        .static_mem = s_dma_mem, .static_len = sizeof(s_dma_mem),
    });
    if (g_net_pool == NULL || g_dma_pool == NULL)
        return -1;
    return 0;
}

void send_packet(const uint8_t* payload, size_t len)
{
    buffer_block_t* blk = buffer_pool_alloc(g_net_pool, len);
    size_t n = 0;
    if (blk == NULL)
        return;
    buffer_block_write(blk, payload, len, &n);
    /* ... hand blk to the network task ... it must call buffer_pool_free(g_net_pool, blk) */
}

void* dma_alloc(size_t len)
{
    buffer_block_t* blk = buffer_pool_alloc(g_dma_pool, len);
    return (blk != NULL) ? blk->data : NULL;   /* access via public field, see note */
}
```

> **Note**: `blk->data` is a public field for zero-copy access; remember to free with `buffer_pool_free(g_dma_pool, blk)`, passing the same pool the block came from.

### Example 3 — ISR context usage

```c
static buffer_pool_t* s_isr_pool;

/* Called from an ISR: atomic operations are interrupt-safe internally */
void rx_isr(const uint8_t* byte)
{
    buffer_block_t* blk = buffer_pool_alloc_isr(s_isr_pool, 1);
    size_t n = 0;
    if (blk == NULL)
        return;                       /* pool busy/exhausted — drop */
    buffer_block_write(blk, byte, 1, &n);
    buffer_pool_free_isr(s_isr_pool, blk);
}
```

### Example 4 — dynamic-only mode

```c
/* Compile with -DCONFIG_BUFFER_POOL_DYNAMIC_ONLY (heap required) */
buffer_pool_t* pool = buffer_pool_create(&(buffer_pool_config_t){
    .name = "scratch",
    .use_static = false,              /* ignored in dynamic-only mode */
});

buffer_block_t* blk = buffer_pool_alloc(pool, 256);   /* straight to calloc */
size_t n = 0;
buffer_block_write(blk, "hello", 5, &n);              /* n == 5 */
buffer_pool_free(pool, blk);                          /* frees the heap chunk */
buffer_pool_destroy(pool);
```

### Example 5 — diagnostics / sizing a pool

```c
/* Before enabling a pool in production, measure peak usage: */
void pool_report(buffer_pool_t* pool)
{
    printf("size=%zu free=%zu used=%u peak=%u\n",
           buffer_pool_size(pool),
           buffer_pool_free_space(pool),
           buffer_pool_used(pool),
           buffer_pool_peak(pool));
    buffer_pool_reset_peak(pool);     /* start a fresh measurement window */
}
```

> **Note**: the pool `name` is stored internally for debugging but not exposed by a getter; `buffer_pool_size` / `free_space` / `used` / `peak` are enough for monitoring and sizing decisions. Run with `buffer_pool_reset_peak()` at boot, then read `peak` after a soak test to size the pool correctly.

## API overview

### Lifecycle
- `buffer_pool_t* buffer_pool_create(const buffer_pool_config_t* config)` — create a pool; NULL on invalid config / out of resources.
- `void buffer_pool_destroy(buffer_pool_t* pool)` — release owned resources.

### Allocation
- `buffer_block_t* buffer_pool_alloc(buffer_pool_t* pool, size_t size)` — allocate a block of `size` data bytes (static first, falls back to dynamic when exhausted).
- `buffer_block_t* buffer_pool_alloc_static(buffer_pool_t* pool, size_t size)` — allocate from the **static pool only**, never dynamic; returns NULL when the static pool cannot satisfy the request.
- `int buffer_pool_expand(buffer_pool_t* pool, void* mem, size_t len)` — **append a memory segment** to the static pool at runtime. Existing blocks never move (no pointer invalidation); the segment is merged into the free list and coalesced with adjacent free blocks. The caller must guarantee `mem` is exclusively owned and does not overlap pool memory.
- `void buffer_pool_free(buffer_pool_t* pool, buffer_block_t* block)` — return the block.
- `buffer_pool_alloc_isr` / `buffer_pool_free_isr` — ISR-safe aliases (internally atomic).

> **Static-only + runtime growth pattern** (never touch the heap):
> ```c
> static _Alignas(8) uint8_t s_pool[2048];
> static _Alignas(8) uint8_t s_extra[1024];   /* reserved expansion segment */
>
> buffer_block_t* alloc_no_heap(size_t n)
> {
>     buffer_block_t* blk = buffer_pool_alloc_static(s_pool, n); /* static only */
>     if (blk == NULL)
>     {
>         buffer_pool_expand(s_pool, s_extra, sizeof(s_extra));  /* grow on demand */
>         blk = buffer_pool_alloc_static(s_pool, n);
>     }
>     return blk;   /* may still be NULL if the extra segment is exhausted too */
> }
> ```

## Segment strategy (smallest-first) and coalescing

The pool tracks a **segment table** (initial pool + every `buffer_pool_expand` segment,
sorted by length ascending, max `BUFF_POOL_MAX_SEGS` = 4). Allocation policy:

1. `buffer_pool_alloc` first checks the threshold: if the request exceeds the largest
   single free block across **all** segments, it goes **straight to dynamic**
   (`calloc`) without touching the static pool.
2. Otherwise allocation walks the segment table **from the smallest segment
   upwards**: the smallest segment is drained first; when it can no longer satisfy
   the request, allocation moves to the next larger segment.
3. `buffer_pool_alloc_static` is the same but never falls back to dynamic.

Coalescing and segments are two complementary layers:

- **Free-list coalescing** (physical layer): when freed blocks are *physically
  adjacent*, they are merged into one large free block regardless of the segment
  table. This is the strongest optimization — adjacent segments share free space.
- **Segment table** (registration layer): records each segment's base/length and
  drives the smallest-first policy.

Consequences:

- **Physically separated segments** (e.g. separate `calloc` chunks): cannot
  coalesce; the smallest-first strategy works precisely, and a request larger
  than any single segment fails static allocation (falls back to dynamic).
- **Physically adjacent segments** (e.g. static arrays the linker happens to
  place next to each other): they coalesce into one large free block, so the
  smallest-first policy is effectively bypassed — but allocation still works
  correctly (blocks are cut from the merged block). A block may then straddle a
  segment-table boundary; this is harmless (blocks only need 8-byte alignment,
  and dual-pointer access requires physical continuity, which the merged block
  provides).

> To make the smallest-first policy visible in tests, keep segments physically
> apart (e.g. allocate each segment separately on the heap); otherwise
> adjacent segments auto-coalesce and the policy is bypassed.

### Block content (dual pointers, ring read/write)
- `int buffer_block_write(buffer_block_t* blk, const void* src, size_t len, size_t* actual)` — write `len` bytes; `*actual` receives the bytes actually written (truncated when full). Returns `0` on success, `-EINVAL` on invalid arguments.
- `int buffer_block_read(buffer_block_t* blk, void* dst, size_t len, size_t* actual)` — read up to `len` bytes; `*actual` receives the bytes actually read (truncated when empty). Returns `0` on success, `-EINVAL` on invalid arguments.
- `size_t buffer_block_used(const buffer_block_t* blk)` — bytes written but not yet read.
- `size_t buffer_block_space(const buffer_block_t* blk)` — remaining writable bytes.
- `void buffer_block_reset(buffer_block_t* blk)` — reset both pointers (clear the block).

> **Return convention**: `buffer_block_write`/`read` use `return` only to report success (`0`) or invalid arguments (`-EINVAL`). Actual byte counts go through the `actual` out-parameter (may be NULL).

### Statistics / diagnostics
- `size_t buffer_pool_size(const buffer_pool_t* pool)` — total static pool bytes (0 when no static pool).
- `size_t buffer_pool_free_space(const buffer_pool_t* pool)` — currently free (splittable) bytes, summed over the free list.
- `uint32_t buffer_pool_used(const buffer_pool_t* pool)` — currently allocated block count.
- `uint32_t buffer_pool_peak(const buffer_pool_t* pool)` — historical peak block count.
- `void buffer_pool_reset_peak(buffer_pool_t* pool)` — reset the peak counter.

## Configuration reference

| Field | Meaning |
|:---|:---|
| `name` | debug label (may be NULL) |
| `use_static` | enable the static pool (ignored in dynamic-only mode) |
| `static_mem` | static pool memory base; NULL = allocate internally (`calloc`) |
| `static_len` | static pool bytes; 0 = use `CONFIG_BUFFER_POOL_SIZE` (default 4096) |

`buffer_pool_size()` vs config: `use_static && static_len > 0` → `static_len`; `use_static && static_len == 0` → `CONFIG_BUFFER_POOL_SIZE`; dynamic-only mode → `0`.

## Notes & limitations

- **Free-list allocation is O(n) first-fit** — suited to low-frequency, variable-size blocks. For high-frequency fixed-size objects prefer a slot/bitmap pool.
- Blocks are 8-byte aligned; for DMA use a pool buffer aligned to the DMA requirement (e.g. `_Alignas(32)`).
- **Who allocates must free**: a block must be returned to the pool it came from (`buffer_block_t.pool` records the owner). Freeing to a different pool is undefined behavior.
- In dynamic-only mode a working heap is required (bare-metal targets must provide `calloc`/`free` or an equivalent).
- The file is gated by `#ifdef CONFIG_BUFFER_POOL`; leave it undefined for standalone use.

## License

Apache-2.0
