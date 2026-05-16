/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT style license that can be
 * found in the LICENSE file.
 *
 * array.h - Generic auto-growing array
 *
 * xArray is a type-erasable dynamic array that stores fixed-size elements
 * in contiguous memory. It automatically grows (doubling capacity) when
 * more space is needed.
 *
 * The array stores elements by value (memcpy'd), so each slot is
 * independently addressable. New slots pushed via xArrayPush() are
 * zero-initialized.
 *
 * Lifecycle callbacks (xArrayCallbacks) let the array automatically
 * manage per-element resources:
 *   - retain:  called when an element is added (via Push / Resize grow).
 *   - release: called when an element is removed (via Pop / Reset / Destroy /
 *              Resize shrink).
 *   - equal:   called by xArrayFind() to compare elements.
 *
 * All callbacks are optional (may be NULL).
 *
 * Typical usage:
 *   xArrayCallbacks cbs = { my_retain, my_release, my_equal };
 *   xArray arr = xArrayCreate(sizeof(MyStruct), 0, &cbs);
 *   MyStruct *slot = (MyStruct *)xArrayPush(&arr);
 *   slot->field = value;
 *   ...
 *   size_t idx = xArrayFind(arr, &key);
 *   ...
 *   xArrayDestroy(arr);
 */

#ifndef XBASE_ARRAY_H
#define XBASE_ARRAY_H

#include <stddef.h>
#include <x/base/base.h>
#include <x/base/error.h>

/* ───────────────────── Types ───────────────────── */

/**
 * @brief Generic auto-growing array (opaque handle).
 *
 * Internally stores: element size, length, capacity, callbacks, and a data
 * buffer. Elements are stored contiguously as raw bytes of elem_size each.
 */
XDEF_HANDLE(xArray);

/**
 * @brief Per-element retain callback.
 *
 * Called when an element is added to the array (via xArrayPush or
 * xArrayResize when growing). The element has already been
 * zero-initialized before this callback is invoked.
 *
 * @param elem  Pointer to the newly added element (type-erased).
 */
typedef void (*xArrayRetainFunc)(void *elem);

/**
 * @brief Per-element release callback.
 *
 * Called when an element is removed from the array (via xArrayPop,
 * xArrayReset, xArrayDestroy, or xArrayResize when shrinking).
 *
 * @param elem  Pointer to the element being removed (type-erased).
 */
typedef void (*xArrayReleaseFunc)(void *elem);

/**
 * @brief Per-element equality callback.
 *
 * Called by xArrayFind() to compare elements.
 *
 * @param elem  Pointer to an element in the array.
 * @param key   Pointer to the key passed to xArrayFind().
 * @return Non-zero if the element matches the key, 0 otherwise.
 */
typedef int (*xArrayEqualFunc)(const void *elem, const void *key);

/**
 * @brief Lifecycle and comparison callbacks for xArray.
 *
 * All fields are optional (may be NULL).
 */
XDEF_STRUCT(xArrayCallbacks) {
  xArrayRetainFunc  retain;
  xArrayReleaseFunc release;
  xArrayEqualFunc   equal;
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a dynamic array.
 *
 * @param elem_size   Size of each element in bytes (must be > 0).
 * @param initial_cap Initial capacity hint in number of elements. 0 for
 * default.
 * @param cbs         Callbacks (may be NULL for no callbacks).
 * @return A new array, or NULL on allocation failure or invalid args.
 */
XCAPI(xArray) xArrayCreate(size_t elem_size, size_t initial_cap,
                           const xArrayCallbacks *cbs);

/**
 * @brief Destroy an array, releasing all memory.
 *
 * If the release callback is set, it is called for each element before
 * the data buffer is freed.
 *
 * @param arr  Array to destroy, or NULL (no-op).
 */
XCAPI(void) xArrayDestroy(xArray arr);

/**
 * @brief Remove all elements but keep the allocated storage for reuse.
 *
 * If the release callback is set, it is called for each element.
 *
 * @param arr  Array to reset (no-op if NULL).
 */
XCAPI(void) xArrayReset(xArray arr);

/* ───────────────────── Mutators ───────────────────── */

/**
 * @brief Append a new zero-initialized element and return a pointer to it.
 *
 * The array grows automatically if the capacity is exhausted.
 * Because growth may realloc, the caller must pass a pointer to the
 * handle so it can be updated.
 *
 * If the retain callback is set, it is called on the new element
 * after zero-initialization.
 *
 * @param arrp  Pointer to the array handle (must not be NULL).
 * @return Pointer to the newly appended element, or NULL on failure.
 */
XCAPI(void *) xArrayPush(xArray *arrp);

/**
 * @brief Remove the last element.
 *
 * If the release callback is set, it is called on the removed element.
 *
 * @param arr  Array (must not be NULL).
 * @return xErrno_Ok on success, xErrno_InvalidState if the array is empty.
 */
XCAPI(xErrno) xArrayPop(xArray arr);

/**
 * @brief Resize the array to @p new_len elements.
 *
 * If growing, new elements are zero-initialized and the retain callback
 * is called for each. If shrinking, the release callback is called for
 * each removed element.
 *
 * @param arrp     Pointer to the array handle (must not be NULL).
 * @param new_len  Desired number of elements.
 * @return xErrno_Ok on success, xErrno_NoMemory on allocation failure.
 */
XCAPI(xErrno) xArrayResize(xArray *arrp, size_t new_len);

/**
 * @brief Remove a contiguous range of elements [start, start+count).
 *
 * The release callback is called for each removed element. Surviving
 * elements after the range are shifted left to fill the gap. The
 * array length is reduced by @p count.
 *
 * @param arr    Array (must not be NULL).
 * @param start  Index of the first element to remove.
 * @param count  Number of elements to remove.
 * @return xErrno_Ok on success, xErrno_InvalidArg if the range is
 *         out of bounds.
 */
XCAPI(xErrno) xArrayRemoveRange(xArray arr, size_t start, size_t count);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Insert an element at @p idx, shifting existing elements right.
 *
 * Elements at index @p idx and beyond are shifted one position to the
 * right to make room. The array grows if needed. If the retain
 * callback is set, it is called on the new element after it is
 * copied into position. If the allocation fails, the array is
 * unchanged and xErrno_NoMemory is returned.
 *
 * @param arrp  Pointer to the array handle (must not be NULL).
 * @param idx   Index at which to insert (0 <= idx <= xArrayLen(*arrp)).
 * @param elem  Pointer to the element to copy in (must not be NULL,
 *              element size must match the array's elem_size).
 * @return xErrno_Ok on success, xErrno_NoMemory on allocation failure,
 *         xErrno_InvalidArg on invalid arguments.
 */
XCAPI(xErrno) xArrayInsert(xArray *arrp, size_t idx, const void *elem);

/**
 * @brief Return a pointer to the element at index @p idx.
 *
 * @param arr  Array.
 * @param idx  Element index.
 * @return Pointer to the element, or NULL if arr is NULL or idx is out of
 * range.
 */
XCAPI(void *) xArrayAt(xArray arr, size_t idx);

/**
 * @brief Return the number of elements currently stored.
 */
XCAPI(size_t) xArrayLen(xArray arr);

/**
 * @brief Return the current capacity (number of elements that can be
 *        stored without reallocation).
 */
XCAPI(size_t) xArrayCap(xArray arr);

/**
 * @brief Return a pointer to the raw element storage.
 *
 * Useful for bulk operations (e.g. memcpy, iteration with pointer arithmetic).
 * The returned pointer is valid until the next mutating call.
 *
 * @param arr  Array.
 * @return Pointer to the first element, or NULL if arr is NULL or empty.
 */
XCAPI(void *) xArrayData(xArray arr);

/**
 * @brief Find the first element matching @p key using the equal callback.
 *
 * @param arr  Array.
 * @param key  Key to compare against (passed as the second argument to equal).
 * @return Index of the matching element, or (size_t)-1 if not found or
 *         no equal callback is set.
 */
XCAPI(size_t) xArrayFind(xArray arr, const void *key);

#endif /* XBASE_ARRAY_H */
