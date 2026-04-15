/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map.h - Generic key-value map
 *
 * A generic map (associative container) that stores opaque key-value
 * pairs and supports multiple backend implementations selected at
 * creation time via xMapType.
 *
 * Backends:
 *   xMapType_Hash  — separate-chaining hash table (default, general purpose)
 *   xMapType_Flat  — open-addressing hash table (cache-friendly, small keys)
 *   xMapType_Tree  — red-black tree (ordered by hash value)
 */

#ifndef XBASE_MAP_H
#define XBASE_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Types
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Map implementation type.
 */
XDEF_ENUM(xMapType){
  xMapType_Hash = 0, /**< Separate-chaining hash table */
  xMapType_Flat,     /**< Open-addressing hash table   */
  xMapType_Tree,     /**< Red-black tree               */
};

/**
 * @brief Opaque handle to a map.
 */
XDEF_HANDLE(xMap);

/**
 * @brief Hash function for map keys.
 * @param key The key to hash.
 * @return A 64-bit hash value.
 */
typedef uint64_t (*xMapHashFunc)(const void *key);

/**
 * @brief Equality function for map keys.
 * @param a First key.
 * @param b Second key.
 * @return true if the keys are equal.
 */
typedef bool (*xMapEqFunc)(const void *a, const void *b);

/**
 * @brief Iterator callback for xMapIterate.
 * @param key The key of the current entry.
 * @param val The value of the current entry.
 * @param arg User-provided argument.
 * @return true to continue iteration, false to stop early.
 */
typedef bool (*xMapIterFunc)(const void *key, void *val, void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a map with the specified backend implementation.
 * @param type Backend type (xMapType_Hash, xMapType_Flat, etc.).
 * @param cap  Initial capacity hint. 0 for default (16).
 * @param hash Hash function for keys (required).
 * @param eq   Equality function for keys (required).
 * @return A new map handle, or NULL on failure.
 */
XCAPI(xMap) xMapCreate(xMapType type, size_t cap, xMapHashFunc hash,
                       xMapEqFunc eq);

/**
 * @brief Destroy a map and free all internal memory.
 *
 * Does NOT free user-stored keys or values.
 * @param m The map to destroy. NULL is a safe no-op.
 */
XCAPI(void) xMapDestroy(xMap m);

/* ═══════════════════════════════════════════════════════════════════
 *  Operations
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Insert or update a key-value pair.
 * @param m   The map.
 * @param key The key (must remain valid while stored).
 * @param val The value to associate with the key.
 * @return xErrno_Ok on success, xErrno_NoMemory if allocation fails.
 */
XCAPI(xErrno) xMapSet(xMap m, const void *key, void *val);

/**
 * @brief Look up a value by key.
 * @param m   The map.
 * @param key The key to search for.
 * @return The associated value, or NULL if not found.
 */
XCAPI(void *) xMapGet(xMap m, const void *key);

/**
 * @brief Remove a key-value pair.
 * @param m   The map.
 * @param key The key to remove.
 * @return The removed value, or NULL if the key was not found.
 */
XCAPI(void *) xMapDel(xMap m, const void *key);

/**
 * @brief Return the number of key-value pairs in the map.
 * @param m The map.
 * @return Number of entries, or 0 if m is NULL.
 */
XCAPI(size_t) xMapLen(xMap m);

/**
 * @brief Iterate over all key-value pairs.
 *
 * The callback is invoked for each entry. If the callback returns
 * false, iteration stops immediately.
 *
 * @param m   The map.
 * @param fn  Iterator callback (required).
 * @param arg User-provided argument forwarded to fn.
 */
XCAPI(void) xMapIterate(xMap m, xMapIterFunc fn, void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  Built-in hash / equality helpers
 * ═══════════════════════════════════════════════════════════════════ */

/** @brief FNV-1a hash for NUL-terminated C strings. */
XCAPI(uint64_t) xMapStrHash(const void *key);

/** @brief strcmp-based equality for C strings. */
XCAPI(bool) xMapStrEq(const void *a, const void *b);

/** @brief Identity hash for integer keys cast to (void *). */
XCAPI(uint64_t) xMapIntHash(const void *key);

/** @brief Pointer-value equality for integer keys cast to (void *). */
XCAPI(bool) xMapIntEq(const void *a, const void *b);

#endif /* XBASE_MAP_H */
