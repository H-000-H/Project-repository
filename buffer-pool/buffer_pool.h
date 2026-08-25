/*
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @file buffer_pool.h
 */

#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Atomic type. The actual atomic operations are wrapped internally in
     * buffer_pool.c (BUFF_POOL_* macros). */
    typedef volatile uint32_t buff_pool_atomic_uint_t;

    /* Allocated block: a ring read/write buffer with dual pointers.
     * head (write) and tail (read) are atomic. */
    typedef struct buffer_block
    {
        struct buffer_pool* pool; /**< owning pool */
        uint8_t* raw; /**< raw address in the pool, or the calloc pointer */
        uint8_t* data; /**< data area start (after raw) */
        size_t capacity; /**< data area capacity in bytes */
        buff_pool_atomic_uint_t head; /**< dual pointer: write index */
        buff_pool_atomic_uint_t tail; /**< dual pointer: read index */
        bool from_static; /**< true = from static pool, false = dynamic */
    } buffer_block_t;

    typedef struct buffer_pool_config
    {
        const char* name; /**< debug label */
        bool use_static; /**< enable the static pool (ignored in dynamic-only mode) */
        void* static_mem; /**< static pool memory base; NULL = allocate internally */
        size_t static_len; /**< static pool bytes; 0 = use default pool size (4096) */
    } buffer_pool_config_t;

    typedef struct buffer_pool buffer_pool_t;

    /* ── Lifecycle ── */
    /**
     * @brief Create a buffer pool (free-list + dual-pointer block allocator)
     * @param[in] config pool configuration (name/use_static/static_mem/static_len)
     * @return pool handle; NULL on invalid config or out of resources
     */
    buffer_pool_t* buffer_pool_create(const buffer_pool_config_t* config);
    /**
     * @brief Destroy a buffer pool and release its resources
     * @param[in] pool pool handle (may be NULL)
     */
    void buffer_pool_destroy(buffer_pool_t* pool);

    /* ── Allocate / Free ── */
    /**
     * @brief Allocate a block of the requested size from the pool
     *        (static first, falls back to dynamic when exhausted;
     *        dynamic-only mode goes straight to dynamic allocation)
     * @param[in] pool pool handle
     * @param[in] size requested data area bytes (caller-specified, not fixed)
     * @return block handle; NULL on failure
     */
    buffer_block_t* buffer_pool_alloc(buffer_pool_t* pool, size_t size);
    /**
     * @brief Allocate a block from the STATIC pool only (never dynamic)
     * @param[in] pool pool handle
     * @param[in] size requested data area bytes
     * @return block handle; NULL when the static pool has no fit (caller may
     *         then buffer_pool_expand() or fall back to its own handling)
     * @note Unlike buffer_pool_alloc, this never falls back to calloc.
     */
    buffer_block_t* buffer_pool_alloc_static(buffer_pool_t* pool, size_t size);
    /**
     * @brief Append a memory segment to the static pool (runtime growth)
     * @param[in] pool pool handle
     * @param[in] mem segment base (must NOT overlap pool memory or any block;
     *                 caller guarantees the memory is exclusively owned)
     * @param[in] len segment bytes (>= header + minimum block)
     * @return 0 = success; -EINVAL = invalid arguments
     * @note Existing blocks are never moved, so no pointer invalidation.
     *       The segment is added to the free list; adjacent free blocks are
     *       coalesced automatically. Never realloc/resize in place.
     */
    int buffer_pool_expand(buffer_pool_t* pool, void* mem, size_t len);
    /**
     * @brief Return a block to the pool (static blocks go back to the free
     *        list with coalescing; dynamic blocks are freed)
     * @param[in] pool pool handle
     * @param[in] block block to free (must come from buffer_pool_alloc of this pool)
     */
    void buffer_pool_free(buffer_pool_t* pool, buffer_block_t* block);

    /* ISR-safe variants (internally atomic / interrupt-disable based) */
    /**
     * @brief Allocate a block from ISR context (same as buffer_pool_alloc)
     * @param[in] pool pool handle
     * @param[in] size requested bytes
     * @return block handle; NULL on failure
     */
    buffer_block_t* buffer_pool_alloc_isr(buffer_pool_t* pool, size_t size);
    /**
     * @brief Free a block from ISR context (same as buffer_pool_free)
     * @param[in] pool pool handle
     * @param[in] block block to free
     */
    void buffer_pool_free_isr(buffer_pool_t* pool, buffer_block_t* block);

    /* ── Block content management (dual pointers, ring read/write;
     *    actual byte counts go through pointers; return only reports status) ── */
    /**
     * @brief Write data into a block (advances head with wraparound; truncates when full)
     * @param[in] block block handle
     * @param[in] src source data
     * @param[in] len requested write bytes
     * @param[out] actual optional: actual written bytes (<= len), may be NULL
     * @return 0 = success; -EINVAL = invalid arguments
     * @note When the block is nearly full the write is truncated to the
     *       remaining space and still returns 0 (success); use *actual to
     *       detect a short write.
     */
    int buffer_block_write(buffer_block_t* block, const void* src, size_t len, size_t* actual);
    /**
     * @brief Read data from a block (advances tail with wraparound; empty returns 0)
     * @param[in] block block handle
     * @param[out] dst destination buffer
     * @param[in] len requested read bytes
     * @param[out] actual optional: actual read bytes (<= len), may be NULL
     * @return 0 = success; -EINVAL = invalid arguments
     * @note When there is less data than requested the read is truncated and
     *       still returns 0 (success); use *actual to detect a short read.
     */
    int buffer_block_read(buffer_block_t* block, void* dst, size_t len, size_t* actual);
    /**
     * @brief Get the number of bytes written but not yet read
     * @param[in] block block handle
     * @return occupied bytes
     */
    size_t buffer_block_used(const buffer_block_t* block);
    /**
     * @brief Get the remaining writable bytes in a block
     * @param[in] block block handle
     * @return free space in bytes
     */
    size_t buffer_block_space(const buffer_block_t* block);
    /**
     * @brief Reset the dual pointers (clears the block content)
     * @param[in] block block handle
     */
    void buffer_block_reset(buffer_block_t* block);

    /* ── Statistics / diagnostics ── */
    /**
     * @brief Get the total pool size (static pool bytes)
     * @param[in] pool pool handle
     * @return total pool bytes; 0 when no static pool is enabled
     */
    size_t buffer_pool_size(const buffer_pool_t* pool);
    /**
     * @brief Get the currently free (splittable) bytes by walking the free list
     * @param[in] pool pool handle
     * @return free bytes; 0 when no static pool is enabled
     */
    size_t buffer_pool_free_space(const buffer_pool_t* pool);
    /**
     * @brief Get the current number of allocated blocks
     * @param[in] pool pool handle
     * @return allocated block count
     */
    uint32_t buffer_pool_used(const buffer_pool_t* pool);
    /**
     * @brief Get the historical peak usage (debug/certification)
     * @param[in] pool pool handle
     * @return peak block count
     */
    uint32_t buffer_pool_peak(const buffer_pool_t* pool);
    /**
     * @brief Reset the peak counter
     * @param[in] pool pool handle
     */
    void buffer_pool_reset_peak(buffer_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* BUFFER_POOL_H */
