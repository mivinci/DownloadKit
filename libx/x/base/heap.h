/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * heap.h - Generic min-heap
 *
 * A generic binary min-heap that stores opaque pointers and orders
 * them via a user-supplied comparison function. Each element carries
 * its heap index so that removal and priority updates are O(log n).
 */

#ifndef XBASE_HEAP_H
#define XBASE_HEAP_H

#include <stddef.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Comparison function for heap elements.
 * @return negative if a < b, 0 if equal, positive if a > b.
 */
typedef int (*xHeapCmpFunc)(const void *a, const void *b);

/**
 * @brief Callback to notify an element of its new heap index.
 *
 * Called whenever an element is moved within the heap. The element
 * should store this index so that xHeapRemove / xHeapUpdate can
 * locate it in O(1).
 *
 * @param elem The element being moved.
 * @param idx  The new index in the heap array.
 */
typedef void (*xHeapSetIdxFunc)(void *elem, size_t idx);

/**
 * @brief Opaque handle to a min-heap.
 */
XDEF_HANDLE(xHeap);

/**
 * @brief Create a min-heap.
 * @param cmp   Comparison function (required).
 * @param setidx Index-update callback (required).
 * @param cap   Initial capacity hint. 0 for default.
 * @return A new heap, or NULL on failure.
 */
XCAPI(xHeap) xHeapCreate(xHeapCmpFunc cmp, xHeapSetIdxFunc setidx, size_t cap);

/**
 * @brief Destroy a heap. Does NOT free the elements themselves.
 * @param h The heap to destroy.
 */
XCAPI(void) xHeapDestroy(xHeap h);

/**
 * @brief Push an element onto the heap.
 * @param h    The heap.
 * @param elem The element to insert.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xHeapPush(xHeap h, void *elem);

/**
 * @brief Peek at the minimum element without removing it.
 * @param h The heap.
 * @return The minimum element, or NULL if empty.
 */
XCAPI(void *) xHeapPeek(xHeap h);

/**
 * @brief Pop the minimum element from the heap.
 * @param h The heap.
 * @return The minimum element, or NULL if empty.
 */
XCAPI(void *) xHeapPop(xHeap h);

/**
 * @brief Remove an element at a known index.
 * @param h   The heap.
 * @param idx The index of the element (maintained via xHeapSetIdxFunc).
 * @return The removed element, or NULL if idx is out of range.
 */
XCAPI(void *) xHeapRemove(xHeap h, size_t idx);

/**
 * @brief Re-heapify after an element's priority has changed.
 * @param h   The heap.
 * @param idx The index of the modified element.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xHeapUpdate(xHeap h, size_t idx);

/**
 * @brief Return the number of elements in the heap.
 * @param h The heap.
 * @return Number of elements.
 */
XCAPI(size_t) xHeapSize(xHeap h);

#endif /* XBASE_HEAP_H */
