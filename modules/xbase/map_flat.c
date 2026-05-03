/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_flat.c - Open-addressing (linear probing) hash table implementation
 */

#include "map_private.h"

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Internal types
 * ═══════════════════════════════════════════════════════════════════ */

enum {
  SLOT_EMPTY     = 0,
  SLOT_OCCUPIED  = 1,
  SLOT_TOMBSTONE = 2,
};

XDEF_STRUCT(xMapFlatSlot) {
  const void *key;
  void       *val;
  int         state;
};

XDEF_STRUCT(xMapFlat) {
  xMapBase      base; /* must be first */
  xMapFlatSlot *slots;
  size_t        size; /* number of OCCUPIED entries */
  size_t        cap;  /* total slot count (always power of 2) */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static inline xMapFlat *self(xMap m) {
  return (xMapFlat *)m;
}

static inline size_t slot_idx(xMapFlat *f, const void *key) {
  return (size_t)(f->base.hash(key) & (uint64_t)(f->cap - 1));
}

static xErrno flat_resize(xMapFlat *f) {
  size_t        new_cap = f->cap * 2;
  xMapFlatSlot *new_slots =
    (xMapFlatSlot *)calloc(new_cap, sizeof(xMapFlatSlot));
  if (!new_slots) return xErrno_NoMemory;

  /* Rehash only OCCUPIED entries (tombstones are discarded) */
  for (size_t i = 0; i < f->cap; i++) {
    if (f->slots[i].state != SLOT_OCCUPIED) continue;

    size_t idx =
      (size_t)(f->base.hash(f->slots[i].key) & (uint64_t)(new_cap - 1));
    while (new_slots[idx].state == SLOT_OCCUPIED) {
      idx = (idx + 1) & (new_cap - 1);
    }
    new_slots[idx].key   = f->slots[i].key;
    new_slots[idx].val   = f->slots[i].val;
    new_slots[idx].state = SLOT_OCCUPIED;
  }

  /* Only free separately allocated slots (not the initial inline array) */
  if (f->slots != (xMapFlatSlot *)(f + 1)) free(f->slots);
  f->slots = new_slots;
  f->cap   = new_cap;
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable implementations
 * ═══════════════════════════════════════════════════════════════════ */

static xErrno flat_set(xMap m, const void *key, void *val) {
  xMapFlat *f = self(m);

  /* Check load factor before inserting (> 70%) */
  if ((f->size + 1) * 10 > f->cap * 7) {
    xErrno err = flat_resize(f);
    if (err != xErrno_Ok) return err;
  }

  size_t idx       = slot_idx(f, key);
  size_t tombstone = (size_t)-1; /* first tombstone seen */

  for (size_t i = 0; i < f->cap; i++) {
    size_t cur = (idx + i) & (f->cap - 1);

    if (f->slots[cur].state == SLOT_EMPTY) {
      /* Key not found — insert at tombstone if we saw one, else here */
      size_t ins          = (tombstone != (size_t)-1) ? tombstone : cur;
      f->slots[ins].key   = key;
      f->slots[ins].val   = val;
      f->slots[ins].state = SLOT_OCCUPIED;
      f->size++;
      return xErrno_Ok;
    }

    if (f->slots[cur].state == SLOT_TOMBSTONE) {
      if (tombstone == (size_t)-1) tombstone = cur;
      continue;
    }

    /* SLOT_OCCUPIED */
    if (f->base.eq(f->slots[cur].key, key)) {
      f->slots[cur].val = val;
      return xErrno_Ok;
    }
  }

  /* Should never reach here if load factor is maintained */
  return xErrno_NoMemory;
}

static void *flat_get(xMap m, const void *key) {
  xMapFlat *f   = self(m);
  size_t    idx = slot_idx(f, key);

  for (size_t i = 0; i < f->cap; i++) {
    size_t cur = (idx + i) & (f->cap - 1);

    if (f->slots[cur].state == SLOT_EMPTY) return NULL;

    if (f->slots[cur].state == SLOT_OCCUPIED &&
        f->base.eq(f->slots[cur].key, key)) {
      return f->slots[cur].val;
    }
    /* TOMBSTONE: keep probing */
  }
  return NULL;
}

static void *flat_del(xMap m, const void *key) {
  xMapFlat *f   = self(m);
  size_t    idx = slot_idx(f, key);

  for (size_t i = 0; i < f->cap; i++) {
    size_t cur = (idx + i) & (f->cap - 1);

    if (f->slots[cur].state == SLOT_EMPTY) return NULL;

    if (f->slots[cur].state == SLOT_OCCUPIED &&
        f->base.eq(f->slots[cur].key, key)) {
      void *val           = f->slots[cur].val;
      f->slots[cur].key   = NULL;
      f->slots[cur].val   = NULL;
      f->slots[cur].state = SLOT_TOMBSTONE;
      f->size--;
      return val;
    }
  }
  return NULL;
}

static size_t flat_len(xMap m) {
  return self(m)->size;
}

static void flat_iterate(xMap m, xMapIterFunc fn, void *arg) {
  xMapFlat *f = self(m);
  for (size_t i = 0; i < f->cap; i++) {
    if (f->slots[i].state == SLOT_OCCUPIED) {
      if (!fn(f->slots[i].key, f->slots[i].val, arg)) return;
    }
  }
}

static void flat_destroy(xMap m) {
  xMapFlat *f = self(m);
  /* slots is only separately allocated after a resize */
  if (f->slots != (xMapFlatSlot *)(f + 1)) free(f->slots);
  free(f);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable instance
 * ═══════════════════════════════════════════════════════════════════ */

static const xMapVTable flat_vtable = {
  .set     = flat_set,
  .get     = flat_get,
  .del     = flat_del,
  .len     = flat_len,
  .iterate = flat_iterate,
  .destroy = flat_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Constructor
 * ═══════════════════════════════════════════════════════════════════ */

xMap xMapFlatCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq) {
  /* Single allocation: struct + initial slot array in contiguous memory */
  xMapFlat *f =
    (xMapFlat *)calloc(1, sizeof(xMapFlat) + cap * sizeof(xMapFlatSlot));
  if (!f) return NULL;

  f->slots       = (xMapFlatSlot *)(f + 1);
  f->base.vtable = &flat_vtable;
  f->base.hash   = hash;
  f->base.eq     = eq;
  f->cap         = cap;
  f->size        = 0;

  return (xMap)f;
}
