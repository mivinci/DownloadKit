/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * heap.c - Generic min-heap implementation
 */

#include <x/base/heap.h>

#include <stdlib.h>

#define HEAP_DEFAULT_CAP 16

struct xHeap_ {
  void          **data;
  size_t          size;
  size_t          cap;
  xHeapCmpFunc    cmp;
  xHeapSetIdxFunc setidx;
};

static inline struct xHeap_ *hp(xHeap h) {
  return (struct xHeap_ *)h;
}

static inline void swap(struct xHeap_ *h, size_t i, size_t j) {
  void *tmp  = h->data[i];
  h->data[i] = h->data[j];
  h->data[j] = tmp;
  h->setidx(h->data[i], i);
  h->setidx(h->data[j], j);
}

static void sift_up(struct xHeap_ *h, size_t i) {
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (h->cmp(h->data[i], h->data[parent]) >= 0) break;
    swap(h, i, parent);
    i = parent;
  }
}

static void sift_down(struct xHeap_ *h, size_t i) {
  for (;;) {
    size_t smallest = i;
    size_t left     = 2 * i + 1;
    size_t right    = 2 * i + 2;

    if (left < h->size && h->cmp(h->data[left], h->data[smallest]) < 0) smallest = left;
    if (right < h->size && h->cmp(h->data[right], h->data[smallest]) < 0) smallest = right;

    if (smallest == i) break;
    swap(h, i, smallest);
    i = smallest;
  }
}

static bool ensure_cap(struct xHeap_ *h) {
  if (h->size < h->cap) return true;

  size_t new_cap  = h->cap ? h->cap * 2 : HEAP_DEFAULT_CAP;
  void **new_data = (void **)realloc(h->data, new_cap * sizeof(void *));
  if (!new_data) return false;

  h->data = new_data;
  h->cap  = new_cap;
  return true;
}

/* ───────────────────── Public API ───────────────────── */

xHeap xHeapCreate(xHeapCmpFunc cmp, xHeapSetIdxFunc setidx, size_t cap) {
  struct xHeap_ *h;

  if (!cmp || !setidx) return NULL;

  h = (struct xHeap_ *)calloc(1, sizeof(struct xHeap_));
  if (!h) return NULL;

  h->cap    = cap ? cap : HEAP_DEFAULT_CAP;
  h->cmp    = cmp;
  h->setidx = setidx;

  h->data = (void **)calloc(h->cap, sizeof(void *));
  if (!h->data) {
    free(h);
    return NULL;
  }

  return h;
}

void xHeapDestroy(xHeap h_) {
  struct xHeap_ *h = hp(h_);
  if (!h) return;
  free(h->data);
  free(h);
}

xErrno xHeapPush(xHeap h_, void *elem) {
  struct xHeap_ *h = hp(h_);
  if (!h || !elem) return xErrno_InvalidArg;
  if (!ensure_cap(h)) return xErrno_NoMemory;

  size_t idx   = h->size;
  h->data[idx] = elem;
  h->setidx(elem, idx);
  h->size++;

  sift_up(h, idx);
  return xErrno_Ok;
}

void *xHeapPeek(xHeap h_) {
  struct xHeap_ *h = hp(h_);
  if (!h || h->size == 0) return NULL;
  return h->data[0];
}

void *xHeapPop(xHeap h_) {
  struct xHeap_ *h = hp(h_);
  if (!h || h->size == 0) return NULL;

  void *min = h->data[0];
  h->size--;

  if (h->size > 0) {
    h->data[0] = h->data[h->size];
    h->setidx(h->data[0], 0);
    sift_down(h, 0);
  }

  return min;
}

void *xHeapRemove(xHeap h_, size_t idx) {
  struct xHeap_ *h = hp(h_);
  if (!h || idx >= h->size) return NULL;

  void *elem = h->data[idx];
  h->size--;

  if (idx == h->size) {
    /* Removed the last element, nothing to fix */
    return elem;
  }

  h->data[idx] = h->data[h->size];
  h->setidx(h->data[idx], idx);

  /* The replacement may need to go up or down */
  sift_up(h, idx);
  sift_down(h, idx);

  return elem;
}

xErrno xHeapUpdate(xHeap h_, size_t idx) {
  struct xHeap_ *h = hp(h_);
  if (!h || idx >= h->size) return xErrno_InvalidArg;

  sift_up(h, idx);
  sift_down(h, idx);
  return xErrno_Ok;
}

size_t xHeapSize(xHeap h_) {
  if (!h_) return 0;
  return hp(h_)->size;
}
