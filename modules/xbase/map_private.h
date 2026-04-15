/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_private.h - Internal vtable and base types for map backends
 */

#ifndef XBASE_MAP_PRIVATE_H
#define XBASE_MAP_PRIVATE_H

#include <xbase/map.h>

/* ═══════════════════════════════════════════════════════════════════
 *  VTable
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Virtual dispatch table for map implementations.
 *
 * Each backend (hash, flat, tree) provides a static instance of this
 * struct. The public API functions forward calls through these pointers.
 */
XDEF_STRUCT(xMapVTable) {
  xErrno (*set)(xMap m, const void *key, void *val);
  void *(*get)(xMap m, const void *key);
  void *(*del)(xMap m, const void *key);
  size_t (*len)(xMap m);
  void (*iterate)(xMap m, xMapIterFunc fn, void *arg);
  void (*destroy)(xMap m);
};

/* ═══════════════════════════════════════════════════════════════════
 *  Base struct (embedded at the start of every backend struct)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Common header shared by all map implementations.
 *
 * Every backend struct must embed xMapBase as its first member so that
 * the public API can safely cast the opaque xMap handle to xMapBase*
 * and access the vtable.
 */
XDEF_STRUCT(xMapBase) {
  const xMapVTable *vtable;
  xMapHashFunc      hash;
  xMapEqFunc        eq;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Backend constructors (called by xMapCreate)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a separate-chaining hash map.
 * @param cap  Initial bucket count (must be > 0, already defaulted).
 * @param hash Hash function.
 * @param eq   Equality function.
 * @return Opaque xMap handle, or NULL on failure.
 */
xMap xMapHashCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq);

/**
 * @brief Create an open-addressing (linear probing) hash map.
 * @param cap  Initial slot count (must be > 0, already defaulted).
 * @param hash Hash function.
 * @param eq   Equality function.
 * @return Opaque xMap handle, or NULL on failure.
 */
xMap xMapFlatCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq);

/**
 * @brief Create a red-black tree map.
 * @param cap  Ignored (tree does not pre-allocate).
 * @param hash Hash function (used for key ordering).
 * @param eq   Equality function (used for collision resolution).
 * @return Opaque xMap handle, or NULL on failure.
 */
xMap xMapTreeCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq);

#endif /* XBASE_MAP_PRIVATE_H */
