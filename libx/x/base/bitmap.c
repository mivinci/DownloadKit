/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bitmap.c - General-purpose bitmap implementation
 */

#include "bitmap.h"

#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────── */

static inline uint32_t bits_to_bytes(uint32_t nbits) {
  return (nbits + 7) / 8;
}

/* Mask out the unused trailing bits in the last byte so they don't
   affect popcount / full / empty checks. */
static inline void sanitise_tail(xBitmap *bm) {
  uint32_t tail = bm->nbits % 8;
  if (tail != 0 && bm->nbytes > 0) {
    bm->data[bm->nbytes - 1] &= (uint8_t)((1u << tail) - 1);
  }
}

/* ── Lifecycle ─────────────────────────────────────────── */

xErrno xBitmapInit(xBitmap *bm, uint32_t nbits) {
  if (!bm || nbits == 0) return xErrno_InvalidArg;

  uint32_t nbytes = bits_to_bytes(nbits);
  uint8_t *data   = (uint8_t *)calloc(nbytes, 1);
  if (!data) return xErrno_NoMemory;

  bm->data   = data;
  bm->nbits  = nbits;
  bm->nbytes = nbytes;
  bm->owned  = true;
  return xErrno_Ok;
}

xErrno xBitmapInitStatic(xBitmap *bm, uint8_t *data, uint32_t nbytes, uint32_t nbits) {
  if (!bm || !data || nbits == 0) return xErrno_InvalidArg;
  if (nbytes < bits_to_bytes(nbits)) return xErrno_InvalidArg;

  bm->data   = data;
  bm->nbits  = nbits;
  bm->nbytes = nbytes;
  bm->owned  = false;
  return xErrno_Ok;
}

void xBitmapFree(xBitmap *bm) {
  if (!bm) return;
  if (bm->owned && bm->data) {
    free(bm->data);
  }
  bm->data   = NULL;
  bm->nbits  = 0;
  bm->nbytes = 0;
  bm->owned  = false;
}

/* ── Single-bit operations ─────────────────────────────── */

void xBitmapSet(xBitmap *bm, uint32_t i) {
  if (!bm || i >= bm->nbits) return;
  bm->data[i / 8] |= (uint8_t)(1u << (i % 8));
}

void xBitmapClear(xBitmap *bm, uint32_t i) {
  if (!bm || i >= bm->nbits) return;
  bm->data[i / 8] &= (uint8_t) ~(1u << (i % 8));
}

bool xBitmapTest(const xBitmap *bm, uint32_t i) {
  if (!bm || i >= bm->nbits) return false;
  return (bm->data[i / 8] & (1u << (i % 8))) != 0;
}

void xBitmapToggle(xBitmap *bm, uint32_t i) {
  if (!bm || i >= bm->nbits) return;
  bm->data[i / 8] ^= (uint8_t)(1u << (i % 8));
}

/* ── Bulk operations ───────────────────────────────────── */

void xBitmapSetAll(xBitmap *bm) {
  if (!bm || !bm->data) return;
  memset(bm->data, 0xFF, bm->nbytes);
  sanitise_tail(bm);
}

void xBitmapClearAll(xBitmap *bm) {
  if (!bm || !bm->data) return;
  memset(bm->data, 0, bm->nbytes);
}

/* Portable popcount for a single byte. */
static inline uint32_t popcount8(uint8_t b) {
  /* Brian Kernighan's bit-counting trick */
  uint32_t c = 0;
  while (b) {
    b &= (uint8_t)(b - 1);
    c++;
  }
  return c;
}

uint32_t xBitmapCount(const xBitmap *bm) {
  if (!bm || !bm->data) return 0;

  uint32_t count = 0;
  for (uint32_t i = 0; i < bm->nbytes; i++) {
    count += popcount8(bm->data[i]);
  }

  /* Mask out trailing bits beyond nbits in the last byte */
  uint32_t tail = bm->nbits % 8;
  if (tail != 0) {
    /* Subtract any bits counted in the unused high positions */
    uint8_t last  = bm->data[bm->nbytes - 1];
    uint8_t mask  = (uint8_t)((1u << tail) - 1);
    uint8_t extra = last & (uint8_t)~mask;
    count -= popcount8(extra);
  }

  return count;
}

bool xBitmapFull(const xBitmap *bm) {
  if (!bm || !bm->data) return false;

  /* Check all complete bytes */
  uint32_t full_bytes = bm->nbits / 8;
  for (uint32_t i = 0; i < full_bytes; i++) {
    if (bm->data[i] != 0xFF) return false;
  }

  /* Check the last partial byte */
  uint32_t tail = bm->nbits % 8;
  if (tail != 0) {
    uint8_t mask = (uint8_t)((1u << tail) - 1);
    if ((bm->data[bm->nbytes - 1] & mask) != mask) return false;
  }

  return true;
}

bool xBitmapEmpty(const xBitmap *bm) {
  if (!bm || !bm->data) return true;

  /* Check all complete bytes */
  uint32_t full_bytes = bm->nbits / 8;
  for (uint32_t i = 0; i < full_bytes; i++) {
    if (bm->data[i] != 0) return false;
  }

  /* Check the last partial byte */
  uint32_t tail = bm->nbits % 8;
  if (tail != 0) {
    uint8_t mask = (uint8_t)((1u << tail) - 1);
    if ((bm->data[bm->nbytes - 1] & mask) != 0) return false;
  }

  return true;
}

/* ── Serialisation ─────────────────────────────────────── */

const uint8_t *xBitmapData(const xBitmap *bm, uint32_t *nbytes) {
  if (!bm || !bm->data) {
    if (nbytes) *nbytes = 0;
    return NULL;
  }
  if (nbytes) *nbytes = bm->nbytes;
  return bm->data;
}
