/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * utf8.c - UTF-8 validation implementation
 */

#include <x/base/utf8.h>

#include <stdint.h>

bool xValidateUtf8(const char *data, size_t len) {
  if (len == 0) return true;
  if (!data) return false;

  const uint8_t *p   = (const uint8_t *)data;
  const uint8_t *end = p + len;

  while (p < end) {
    uint8_t b = *p;

    /* ASCII fast path */
    if (b < 0x80) {
      p++;
      continue;
    }

    int seq_len;
    if ((b & 0xE0) == 0xC0)
      seq_len = 2;
    else if ((b & 0xF0) == 0xE0)
      seq_len = 3;
    else if ((b & 0xF8) == 0xF0)
      seq_len = 4;
    else
      return false; /* invalid lead byte (0x80..0xBF or 0xF8..0xFF) */

    /* enough bytes remaining? */
    if (p + seq_len > end) return false;

    /* continuation bytes must be 10xxxxxx */
    for (int i = 1; i < seq_len; i++) {
      if ((p[i] & 0xC0) != 0x80) return false;
    }

    /* reject overlong encodings */
    if (seq_len == 2 && b < 0xC2) return false;
    if (seq_len == 3 && b == 0xE0 && p[1] < 0xA0) return false;
    if (seq_len == 4 && b == 0xF0 && p[1] < 0x90) return false;

    /* reject surrogates (U+D800..U+DFFF) */
    if (seq_len == 3 && b == 0xED && p[1] > 0x9F) return false;

    /* reject > U+10FFFF */
    if (seq_len == 4 && b == 0xF4 && p[1] > 0x8F) return false;
    if (seq_len == 4 && b > 0xF4) return false;

    p += seq_len;
  }

  return true;
}
