/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT style license that can be
 * found in the LICENSE file.
 *
 * array.c - Generic auto-growing array implementation
 */

#include <xbase/array.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Types ───────────────────── */

XDEF_STRUCT(xArray_) {
  size_t            elem_size;
  size_t            len;
  size_t            cap;
  xArrayCallbacks   cbs;
  char              data[];
};

/* ───────────────────── Internal ───────────────────── */

#define ARRAY_DEFAULT_CAP 8
#define ARRAY_NPOS       ((size_t)-1)

static inline struct xArray_ *ar(xArray a) {
  return (struct xArray_ *)a;
}

static size_t arr_next_cap(size_t current, size_t needed) {
  size_t cap = current ? current : ARRAY_DEFAULT_CAP;
  if (needed > SIZE_MAX / 2) return SIZE_MAX;
  while (cap < needed)
    cap *= 2;
  return cap;
}

static void arr_call_retain(struct xArray_ *a, void *elem) {
  if (a->cbs.retain) a->cbs.retain(elem);
}

static void arr_call_release(struct xArray_ *a, void *elem) {
  if (a->cbs.release) a->cbs.release(elem);
}

/**
 * Ensure the array can hold at least `needed` total elements.
 * May realloc the entire object (header + data), so the caller's
 * pointer is updated via arrp.
 */
static xErrno arr_grow(struct xArray_ **arrp, size_t needed) {
  struct xArray_ *a    = *arrp;
  size_t          newcap;
  struct xArray_ *newarr;

  if (needed <= a->cap) return xErrno_Ok;

  newcap  = arr_next_cap(a->cap, needed);
  newarr  = (struct xArray_ *)realloc(a, sizeof(struct xArray_) + newcap * a->elem_size);
  if (!newarr) return xErrno_NoMemory;

  newarr->cap = newcap;
  *arrp       = newarr;
  return xErrno_Ok;
}

/* ───────────────────── Lifecycle ───────────────────── */

xArray xArrayCreate(size_t elem_size, size_t initial_cap,
                    const xArrayCallbacks *cbs) {
  struct xArray_ *a;

  if (elem_size == 0) return NULL;

  if (initial_cap == 0) initial_cap = ARRAY_DEFAULT_CAP;

  a = (struct xArray_ *)malloc(sizeof(struct xArray_) + initial_cap * elem_size);
  if (!a) return NULL;

  a->elem_size = elem_size;
  a->len       = 0;
  a->cap       = initial_cap;
  a->cbs       = cbs ? *cbs : (xArrayCallbacks){NULL, NULL, NULL};
  memset(a->data, 0, initial_cap * elem_size);
  return (xArray)a;
}

void xArrayDestroy(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a) return;

  for (size_t i = 0; i < a->len; i++) {
    arr_call_release(a, a->data + i * a->elem_size);
  }
  free(a);
}

void xArrayReset(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a) return;

  for (size_t i = 0; i < a->len; i++) {
    arr_call_release(a, a->data + i * a->elem_size);
  }
  a->len = 0;
}

/* ───────────────────── Mutators ───────────────────── */

void *xArrayPush(xArray *arrp) {
  struct xArray_ *a;
  xErrno          err;

  if (!arrp || !*arrp) return NULL;

  a   = ar(*arrp);
  err = arr_grow(&a, a->len + 1);
  if (err != xErrno_Ok) return NULL;

  *arrp = (xArray)a;

  void *slot = a->data + a->len * a->elem_size;
  memset(slot, 0, a->elem_size);
  a->len++;
  arr_call_retain(a, slot);
  return slot;
}

xErrno xArrayPop(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a || a->len == 0) return xErrno_InvalidState;

  a->len--;
  arr_call_release(a, a->data + a->len * a->elem_size);
  return xErrno_Ok;
}

xErrno xArrayResize(xArray *arrp, size_t new_len) {
  struct xArray_ *a;
  xErrno          err;

  if (!arrp || !*arrp) return xErrno_InvalidArg;

  a = ar(*arrp);

  /* Shrink: release removed elements */
  if (new_len < a->len) {
    for (size_t i = new_len; i < a->len; i++) {
      arr_call_release(a, a->data + i * a->elem_size);
    }
    a->len = new_len;
    return xErrno_Ok;
  }

  /* Grow: ensure capacity */
  if (new_len > a->cap) {
    err = arr_grow(&a, new_len);
    if (err != xErrno_Ok) return err;
    *arrp = (xArray)a;
  }

  /* Zero-initialize and retain the newly added slots */
  if (new_len > a->len) {
    size_t old_len = a->len;
    memset(a->data + old_len * a->elem_size, 0,
           (new_len - old_len) * a->elem_size);
    a->len = new_len;
    for (size_t i = old_len; i < new_len; i++) {
      arr_call_retain(a, a->data + i * a->elem_size);
    }
  }
  return xErrno_Ok;
}

xErrno xArrayRemoveRange(xArray arr, size_t start, size_t count) {
  struct xArray_ *a = ar(arr);
  if (!a) return xErrno_InvalidArg;
  if (count == 0) return xErrno_Ok;
  if (start + count > a->len) return xErrno_InvalidArg;

  /* Release the removed elements. */
  for (size_t i = start; i < start + count; i++) {
    arr_call_release(a, a->data + i * a->elem_size);
  }

  /* Shift surviving elements left. */
  if (start + count < a->len) {
    memmove(a->data + start * a->elem_size,
            a->data + (start + count) * a->elem_size,
            (a->len - start - count) * a->elem_size);
  }
  a->len -= count;
  return xErrno_Ok;
}

xErrno xArrayInsert(xArray *arrp, size_t idx, const void *elem) {
  struct xArray_ *a;
  xErrno          err;

  if (!arrp || !*arrp || !elem) return xErrno_InvalidArg;

  a = ar(*arrp);

  if (idx > a->len) return xErrno_InvalidArg;

  /* Grow by one. */
  err = arr_grow(&a, a->len + 1);
  if (err != xErrno_Ok) return err;

  *arrp = (xArray)a;

  /* Shift elements right to make room at idx. */
  if (idx < a->len) {
    memmove(a->data + (idx + 1) * a->elem_size,
            a->data + idx * a->elem_size,
            (a->len - idx) * a->elem_size);
  }

  /* Copy the new element into position. */
  memcpy(a->data + idx * a->elem_size, elem, a->elem_size);

  /* Update length. */
  a->len++;

  /* Retain the newly inserted element. */
  arr_call_retain(a, a->data + idx * a->elem_size);

  return xErrno_Ok;
}

/* ───────────────────── Accessors ───────────────────── */

void *xArrayAt(xArray arr, size_t idx) {
  struct xArray_ *a = ar(arr);
  if (!a || idx >= a->len) return NULL;
  return a->data + idx * a->elem_size;
}

size_t xArrayLen(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a) return 0;
  return a->len;
}

size_t xArrayCap(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a) return 0;
  return a->cap;
}

void *xArrayData(xArray arr) {
  struct xArray_ *a = ar(arr);
  if (!a || a->len == 0) return NULL;
  return a->data;
}

size_t xArrayFind(xArray arr, const void *key) {
  struct xArray_ *a = ar(arr);
  if (!a || !a->cbs.equal || !key) return ARRAY_NPOS;

  for (size_t i = 0; i < a->len; i++) {
    if (a->cbs.equal(a->data + i * a->elem_size, key))
      return i;
  }
  return ARRAY_NPOS;
}
