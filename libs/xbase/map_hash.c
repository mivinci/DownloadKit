/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_hash.c - Separate-chaining hash table implementation
 */

#include "map_private.h"

#include <stdlib.h>

#include <xbase/slab.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Internal types
 * ═══════════════════════════════════════════════════════════════════ */

XDEF_STRUCT(xMapHashEntry) {
  const void    *key;
  void          *val;
  xMapHashEntry *next;
};

XDEF_STRUCT(xMapHash) {
  xMapBase        base; /* must be first */
  xMapHashEntry **buckets;
  size_t          size;
  size_t          cap;
  xSlab          *entry_pool; /* xMapHashEntry pool */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static inline xMapHash *self(xMap m) {
  return (xMapHash *)m;
}

static inline size_t bucket_idx(xMapHash *h, const void *key) {
  return (size_t)(h->base.hash(key) & (uint64_t)(h->cap - 1));
}

static xErrno hash_resize(xMapHash *h) {
  size_t          new_cap = h->cap * 2;
  xMapHashEntry **new_buckets =
    (xMapHashEntry **)calloc(new_cap, sizeof(xMapHashEntry *));
  if (!new_buckets) return xErrno_NoMemory;

  /* Rehash all entries into the new bucket array */
  for (size_t i = 0; i < h->cap; i++) {
    xMapHashEntry *e = h->buckets[i];
    while (e) {
      xMapHashEntry *next = e->next;
      size_t idx = (size_t)(h->base.hash(e->key) & (uint64_t)(new_cap - 1));
      e->next    = new_buckets[idx];
      new_buckets[idx] = e;
      e                = next;
    }
  }

  /* Only free separately allocated buckets (not the initial inline array) */
  if (h->buckets != (xMapHashEntry **)(h + 1)) free(h->buckets);
  h->buckets = new_buckets;
  h->cap     = new_cap;
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable implementations
 * ═══════════════════════════════════════════════════════════════════ */

static xErrno hash_set(xMap m, const void *key, void *val) {
  xMapHash *h   = self(m);
  size_t    idx = bucket_idx(h, key);

  /* Search for existing key */
  for (xMapHashEntry *e = h->buckets[idx]; e; e = e->next) {
    if (h->base.eq(e->key, key)) {
      e->val = val;
      return xErrno_Ok;
    }
  }

  /* Check load factor before inserting */
  if (h->size + 1 > h->cap * 3 / 4) {
    xErrno err = hash_resize(h);
    if (err != xErrno_Ok) return err;
    idx = bucket_idx(h, key); /* recalculate after resize */
  }

  /* Insert new entry at head of chain */
  xMapHashEntry *e = (xMapHashEntry *)xSlabAlloc(h->entry_pool);
  if (!e) return xErrno_NoMemory;

  e->key          = key;
  e->val          = val;
  e->next         = h->buckets[idx];
  h->buckets[idx] = e;
  h->size++;
  return xErrno_Ok;
}

static void *hash_get(xMap m, const void *key) {
  xMapHash *h   = self(m);
  size_t    idx = bucket_idx(h, key);

  for (xMapHashEntry *e = h->buckets[idx]; e; e = e->next) {
    if (h->base.eq(e->key, key)) return e->val;
  }
  return NULL;
}

static void *hash_del(xMap m, const void *key) {
  xMapHash      *h    = self(m);
  size_t         idx  = bucket_idx(h, key);
  xMapHashEntry *prev = NULL;

  for (xMapHashEntry *e = h->buckets[idx]; e; prev = e, e = e->next) {
    if (h->base.eq(e->key, key)) {
      void *val = e->val;
      if (prev)
        prev->next = e->next;
      else
        h->buckets[idx] = e->next;
      xSlabFree(h->entry_pool, e);
      h->size--;
      return val;
    }
  }
  return NULL;
}

static size_t hash_len(xMap m) {
  return self(m)->size;
}

static void hash_iterate(xMap m, xMapIterFunc fn, void *arg) {
  xMapHash *h = self(m);
  for (size_t i = 0; i < h->cap; i++) {
    for (xMapHashEntry *e = h->buckets[i]; e; e = e->next) {
      if (!fn(e->key, e->val, arg)) return;
    }
  }
}

static void hash_destroy(xMap m) {
  xMapHash *h = self(m);
  /* xSlabDestroy frees all entries in one shot; map does not own key/val. */
  xSlabDestroy(h->entry_pool);
  /* buckets is only separately allocated after a resize */
  if (h->buckets != (xMapHashEntry **)(h + 1)) free(h->buckets);
  free(h);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable instance
 * ═══════════════════════════════════════════════════════════════════ */

static const xMapVTable hash_vtable = {
  .set     = hash_set,
  .get     = hash_get,
  .del     = hash_del,
  .len     = hash_len,
  .iterate = hash_iterate,
  .destroy = hash_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Constructor
 * ═══════════════════════════════════════════════════════════════════ */

xMap xMapHashCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq) {
  /* Single allocation: struct + initial bucket array in contiguous memory */
  xMapHash *h =
    (xMapHash *)calloc(1, sizeof(xMapHash) + cap * sizeof(xMapHashEntry *));
  if (!h) return NULL;

  /* Per-map slab: single-threaded, no atomics overhead.
   * chunk_bytes = 0 ⇒ slab picks a sensible default. */
  h->entry_pool = xSlabCreate(sizeof(xMapHashEntry), 0, 0);
  if (!h->entry_pool) {
    free(h);
    return NULL;
  }

  h->buckets     = (xMapHashEntry **)(h + 1);
  h->base.vtable = &hash_vtable;
  h->base.hash   = hash;
  h->base.eq     = eq;
  h->cap         = cap;
  h->size        = 0;

  return (xMap)h;
}
