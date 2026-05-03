/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bitmap.h - General-purpose bitmap (bitset)
 */

#ifndef XBASE_BITMAP_H
#define XBASE_BITMAP_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdint.h>

/**
 * @defgroup xBitmap Bitmap
 * @brief A dynamically-sized bitmap (bitset) backed by a contiguous byte array.
 *
 * Bits are numbered starting from 0. Bit i is stored in byte (i / 8),
 * at position (i % 8) from the LSB.
 * @{
 */

/**
 * @brief Bitmap structure.
 *
 * The bitmap owns its internal byte buffer. Use xBitmapInit / xBitmapFree
 * for lifecycle management, or xBitmapInitStatic to wrap an external buffer.
 */
XDEF_STRUCT(xBitmap) {
  uint8_t *data;   /**< Backing byte array                          */
  uint32_t nbits;  /**< Total number of bits                        */
  uint32_t nbytes; /**< Length of @c data in bytes = ceil(nbits/8)  */
  bool     owned;  /**< True if @c data was allocated by xBitmapInit */
};

/* ── Lifecycle ─────────────────────────────────────────── */

/**
 * @brief Initialise a bitmap with @p nbits bits, all cleared to 0.
 * @param bm    Pointer to an uninitialised xBitmap.
 * @param nbits Number of bits (must be > 0).
 * @return xErrno_Ok on success, xErrno_InvalidArg or xErrno_NoMemory on
 * failure.
 */
XCAPI(xErrno) xBitmapInit(xBitmap *bm, uint32_t nbits);

/**
 * @brief Wrap an existing byte buffer as a bitmap (no allocation).
 * @param bm     Pointer to an uninitialised xBitmap.
 * @param data   External buffer (must live as long as the bitmap is used).
 * @param nbytes Length of @p data in bytes.
 * @param nbits  Logical number of bits (<= nbytes * 8).
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xBitmapInitStatic(xBitmap *bm, uint8_t *data, uint32_t nbytes,
                                uint32_t nbits);

/**
 * @brief Free the bitmap's internal buffer (no-op for static bitmaps).
 */
XCAPI(void) xBitmapFree(xBitmap *bm);

/* ── Single-bit operations ─────────────────────────────── */

/**
 * @brief Set bit @p i to 1.
 */
XCAPI(void) xBitmapSet(xBitmap *bm, uint32_t i);

/**
 * @brief Clear bit @p i to 0.
 */
XCAPI(void) xBitmapClear(xBitmap *bm, uint32_t i);

/**
 * @brief Test whether bit @p i is set.
 * @return true if bit @p i is 1, false otherwise.
 */
XCAPI(bool) xBitmapTest(const xBitmap *bm, uint32_t i);

/**
 * @brief Toggle bit @p i.
 */
XCAPI(void) xBitmapToggle(xBitmap *bm, uint32_t i);

/* ── Bulk operations ───────────────────────────────────── */

/**
 * @brief Set all bits to 1.
 */
XCAPI(void) xBitmapSetAll(xBitmap *bm);

/**
 * @brief Clear all bits to 0.
 */
XCAPI(void) xBitmapClearAll(xBitmap *bm);

/**
 * @brief Count the number of bits that are set (popcount).
 */
XCAPI(uint32_t) xBitmapCount(const xBitmap *bm);

/**
 * @brief Return true if all bits are set.
 */
XCAPI(bool) xBitmapFull(const xBitmap *bm);

/**
 * @brief Return true if no bits are set.
 */
XCAPI(bool) xBitmapEmpty(const xBitmap *bm);

/* ── Serialisation ─────────────────────────────────────── */

/**
 * @brief Return a read-only pointer to the raw byte array.
 * @param bm      The bitmap.
 * @param nbytes  [out] Receives the byte length.
 * @return Pointer to the internal data, or NULL on error.
 */
XCAPI(const uint8_t *) xBitmapData(const xBitmap *bm, uint32_t *nbytes);

/** @} */

#endif // XBASE_BITMAP_H
