/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map.c - Public API dispatch and built-in hash/eq helpers
 */

#include "map_private.h"

#include <stdint.h>
#include <string.h>

#define MAP_DEFAULT_CAP 16

/* ═══════════════════════════════════════════════════════════════════
 *  Inline helper
 * ═══════════════════════════════════════════════════════════════════ */

static inline xMapBase *base(xMap m) {
  return (xMapBase *)m;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

xMap xMapCreate(xMapType type, size_t cap, xMapHashFunc hash, xMapEqFunc eq) {
  if (!hash || !eq) return NULL;
  if (cap == 0) cap = MAP_DEFAULT_CAP;

  switch (type) {
  case xMapType_Hash:
    return xMapHashCreate(cap, hash, eq);
  case xMapType_Flat:
    return xMapFlatCreate(cap, hash, eq);
  case xMapType_Tree:
    return xMapTreeCreate(cap, hash, eq);
  default:
    return NULL;
  }
}

void xMapDestroy(xMap m) {
  if (!m) return;
  base(m)->vtable->destroy(m);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Operations (vtable dispatch)
 * ═══════════════════════════════════════════════════════════════════ */

xErrno xMapSet(xMap m, const void *key, void *val) {
  if (!m) return xErrno_InvalidArg;
  return base(m)->vtable->set(m, key, val);
}

void *xMapGet(xMap m, const void *key) {
  if (!m) return NULL;
  return base(m)->vtable->get(m, key);
}

void *xMapDel(xMap m, const void *key) {
  if (!m) return NULL;
  return base(m)->vtable->del(m, key);
}

size_t xMapLen(xMap m) {
  if (!m) return 0;
  return base(m)->vtable->len(m);
}

void xMapIterate(xMap m, xMapIterFunc fn, void *arg) {
  if (!m || !fn) return;
  base(m)->vtable->iterate(m, fn, arg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Built-in hash / equality helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* FNV-1a 64-bit */
uint64_t xMapStrHash(const void *key) {
  const char *s = (const char *)key;
  uint64_t    h = 14695981039346656037ULL;
  while (*s) {
    h ^= (uint64_t)(unsigned char)*s++;
    h *= 1099511628211ULL;
  }
  return h;
}

bool xMapStrEq(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b) == 0;
}

/* Splitmix64-style finalizer for integer keys stored as (void *) */
uint64_t xMapIntHash(const void *key) {
  uint64_t x = (uint64_t)(uintptr_t)key;
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

bool xMapIntEq(const void *a, const void *b) {
  return a == b;
}
