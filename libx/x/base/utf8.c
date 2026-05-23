/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * utf8.c - UTF-8 validation via Höhrmann DFA
 *
 * Based on Björn Höhrmann's branchless UTF-8 decoder/validator.
 * Reference: https://bjoern.hoehrmann.de/utf-8/decoder/dfa/
 *
 * The inner loop is two table lookups per byte with no branches,
 * giving ~3-4 GB/s throughput on modern CPUs. An ASCII fast-path
 * skips aligned runs of 8 ASCII bytes at a time.
 */

#include <x/base/utf8.h>

#include <stdint.h>

/*
 * Byte classification table (256 entries) and state transition table.
 *
 * Each byte maps to one of 12 character classes (0–11). The state
 * machine starts in state 0 (accept). After processing each byte,
 * the new state = utf8d[256 + state + class]. State 0 = valid
 * codepoint boundary; state 12 = reject (permanently stuck).
 *
 * The classification encodes all UTF-8 structural rules:
 *   - Overlong sequences  → lead byte maps to a class that transitions
 *     to reject state unless followed by the right continuation range
 *   - Surrogates (U+D800..U+DFFF) → ED followed by A0..BF → reject
 *   - Above U+10FFFF → F4 followed by 90..BF → reject, F5+ → reject
 */
/* clang-format off */
static const uint8_t utf8d[] = {
  /* byte → class (0x00..0xFF) */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
   8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,

  /* state × class → next state (9 states × 12 classes = 108 entries) */
  /* state  0 (accept) */   0,12,24,36,60,96,84,12,12,12,48,72,
  /* state 12 (reject) */  12,12,12,12,12,12,12,12,12,12,12,12,
  /* state 24 */           12, 0,12,12,12,12,12, 0,12, 0,12,12,
  /* state 36 */           12,24,12,12,12,12,12,24,12,24,12,12,
  /* state 48 */           12,12,12,12,12,12,12,24,12,12,12,12,
  /* state 60 */           12,24,12,12,12,12,12,12,12,24,12,12,
  /* state 72 */           12,12,12,12,12,12,12,12,12,36,12,12,
  /* state 84 */           12,36,12,12,12,12,12,36,12,36,12,12,
  /* state 96 */           12,36,12,12,12,12,12,12,12,12,12,12,
};
/* clang-format on */

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

bool xValidateUtf8(const char *data, size_t len) {
  if (len == 0) return true;
  if (!data) return false;

  const uint8_t *p   = (const uint8_t *)data;
  const uint8_t *end = p + len;
  uint32_t       state = UTF8_ACCEPT;

  /* ASCII fast-path: skip 8-byte aligned chunks of pure ASCII. */
  while (p + 8 <= end) {
    uint64_t w;
    __builtin_memcpy(&w, p, 8);
    if (w & UINT64_C(0x8080808080808080)) break;
    p += 8;
  }

  /* Byte-by-byte DFA for the remainder (or non-ASCII regions). */
  while (p < end) {
    uint32_t byte_class = utf8d[*p];
    state = utf8d[256 + state + byte_class];
    if (state == UTF8_REJECT) return false;
    p++;
  }

  return state == UTF8_ACCEPT;
}
