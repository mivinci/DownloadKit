/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slab.h - Fixed-size object pool (slab allocator)
 *
 * xSlab carves large chunks of memory into fixed-size slots and hands
 * them out via an intrusive freelist.  It is designed to replace the
 * many small `calloc(1, sizeof(T))` / `free()` call sites scattered
 * throughout xbase where objects are allocated and freed at very high
 * frequency (event sources, timers, tree nodes, hash entries, ...).
 *
 * Two variants are provided:
 *
 *   xSlab    — single-threaded, zero synchronisation overhead.
 *              Use this when the pool is owned by a single thread
 *              (e.g. an event loop's internal bookkeeping).
 *
 *   xSlabMt  — multi-threaded, lock-free Treiber stack freelist.
 *              Use this when allocations and frees may come from
 *              different threads (e.g. cross-thread work submission).
 *
 * Both variants:
 *   - Never shrink or return individual slots to the system.  Memory
 *     is only released when the pool itself is destroyed.
 *   - Return uninitialised memory (callers should memset if they
 *     previously relied on calloc's zeroing).
 *   - Honour a configurable alignment (default 16 bytes).
 *
 * Large chunks are acquired via the platform's native anonymous
 * mapping facility when available (mmap on POSIX, VirtualAlloc on
 * Windows) and fall back to malloc otherwise.
 */

#ifndef XBASE_SLAB_H
#define XBASE_SLAB_H

#include <stddef.h>
#include <xbase/base.h>

/** @brief Default slot alignment when 0 is passed to xSlabCreate /
 * xSlabMtCreate. */
#define XSLAB_DEFAULT_ALIGN ((size_t)16)

/** @brief Default chunk size when 0 is passed. 64 KiB. */
#define XSLAB_DEFAULT_CHUNK_BYTES ((size_t)(64 * 1024))

/* ─────────────────────── xSlab (single-threaded) ─────────────────────── */

XDEF_HANDLE(xSlab);

/**
 * @brief Create a single-threaded fixed-size object pool.
 * @ingroup xSlab
 *
 * @param obj_size    Size of each object in bytes. Must be > 0.
 * @param obj_align   Required alignment. 0 selects XSLAB_DEFAULT_ALIGN.
 *                    Must be a power of two when non-zero.
 * @param chunk_bytes Size of each underlying chunk. 0 selects
 *                    XSLAB_DEFAULT_CHUNK_BYTES. The value is rounded
 *                    up to hold at least one slot.
 * @return Newly created pool, or NULL on invalid arguments / OOM.
 */
XCAPI(xSlab *)
xSlabCreate(size_t obj_size, size_t obj_align, size_t chunk_bytes);

/**
 * @brief Destroy a pool and release all its memory.
 * @ingroup xSlab
 *
 * All outstanding pointers handed out by xSlabAlloc become invalid.
 * Passing NULL is a no-op.
 */
XCAPI(void) xSlabDestroy(xSlab *s);

/**
 * @brief Allocate one slot. Contents are uninitialised.
 * @ingroup xSlab
 * @return Pointer to a slot of at least `obj_size` bytes, aligned to
 *         `obj_align`, or NULL on allocation failure.
 */
XCAPI(void *) xSlabAlloc(xSlab *s);

/**
 * @brief Return a slot to the pool. Passing NULL is a no-op.
 * @ingroup xSlab
 *
 * The caller must not touch the slot after this call.
 */
XCAPI(void) xSlabFree(xSlab *s, void *p);

/**
 * @brief Return all outstanding slots to the pool without freeing
 *        the underlying chunks.
 * @ingroup xSlab
 *
 * This is a fast bulk-reclaim used when the caller knows no slot is
 * still in use (e.g. destroying an event loop).  Preserves the
 * chunks so the pool can be reused without touching the allocator.
 */
XCAPI(void) xSlabReset(xSlab *s);

/**
 * @brief Query the number of slots currently handed out (live).
 * @ingroup xSlab
 */
XCAPI(size_t) xSlabInUse(const xSlab *s);

/**
 * @brief Query the slot size configured for this pool.
 * @ingroup xSlab
 */
XCAPI(size_t) xSlabSlotSize(const xSlab *s);

/* ─────────────────────── xSlabMt (multi-threaded) ─────────────────────── */

XDEF_HANDLE(xSlabMt);

/**
 * @brief Create a multi-threaded fixed-size object pool.
 * @ingroup xSlab
 *
 * Chunk acquisition is serialised internally; the fast path
 * (alloc from freelist, free to freelist) is lock-free.
 *
 * @see xSlabCreate for parameter semantics.
 */
XCAPI(xSlabMt *)
xSlabMtCreate(size_t obj_size, size_t obj_align, size_t chunk_bytes);

/** @brief Destroy a multi-threaded pool. Must be externally quiesced. */
XCAPI(void) xSlabMtDestroy(xSlabMt *s);

/** @brief Allocate one slot (uninitialised). Thread-safe. */
XCAPI(void *) xSlabMtAlloc(xSlabMt *s);

/** @brief Return a slot to the pool. Thread-safe. */
XCAPI(void) xSlabMtFree(xSlabMt *s, void *p);

/** @brief Query the slot size configured for this pool. */
XCAPI(size_t) xSlabMtSlotSize(const xSlabMt *s);

#endif /* XBASE_SLAB_H */
