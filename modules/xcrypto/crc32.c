/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * crc32.c - CRC-32 implementation (ISO 3309 / ITU-T V.42)
 *
 * Uses the standard polynomial 0xEDB88320 (reflected).
 * Pure-C implementation, no external dependencies.
 */

#include "crc32.h"

/* ───────────────────── Lookup Table ───────────────────── */

static uint32_t crc32_compute_entry(uint32_t index) {
  uint32_t crc = index;
  for (int j = 0; j < 8; j++) {
    if (crc & 1)
      crc = (crc >> 1) ^ 0xEDB88320u;
    else
      crc = crc >> 1;
  }
  return crc;
}

static int      crc32_table_initialized = 0;
static uint32_t crc32_table[256];

static void crc32_init_table(void) {
  if (crc32_table_initialized) return;
  for (uint32_t i = 0; i < 256; i++) {
    crc32_table[i] = crc32_compute_entry(i);
  }
  crc32_table_initialized = 1;
}

/* ───────────────────── Public API ───────────────────── */

uint32_t xCrc32(const uint8_t *data, size_t len) {
  crc32_init_table();
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}
