/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hex.c - Hex encoding and decoding
 */

#include <xbase/hex.h>

#include <string.h>

/* Map a hex character (0-9, a-f, A-F) to its numeric value.
 * Returns -1 for invalid input. */
static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int xHexEncode(const uint8_t *src, size_t src_len, char *dst, size_t *dst_len) {
  if (!dst || !dst_len) return -1;
  if (!src && src_len > 0) return -1;

  /* Encoded length is exactly 2 * src_len. */
  size_t need = src_len * 2;
  if (*dst_len < need + 1) return -1;

  if (src_len == 0) {
    dst[0]   = '\0';
    *dst_len = 0;
    return 0;
  }

  /* Encode each byte as two lower-case hex digits. */
  static const char kHexDigits[] = "0123456789abcdef";
  size_t            out          = 0;
  for (size_t i = 0; i < src_len; i++) {
    dst[out++] = kHexDigits[(src[i] >> 4) & 0x0F];
    dst[out++] = kHexDigits[src[i] & 0x0F];
  }
  dst[out] = '\0';
  *dst_len = out;
  return 0;
}

int xHexDecode(const char *src, size_t src_len, uint8_t *dst, size_t *dst_len) {
  if (!src || !dst || !dst_len) return -1;

  /* An odd-length input cannot be a valid hex string. */
  if (src_len == 0) {
    *dst_len = 0;
    return 0;
  }
  if ((src_len & 1) != 0) return -1;

  size_t need = src_len / 2;
  if (*dst_len < need) return -1;

  for (size_t i = 0; i < need; i++) {
    int hi = hex_val(src[i * 2]);
    int lo = hex_val(src[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    dst[i] = (uint8_t)((hi << 4) | lo);
  }
  *dst_len = need;
  return 0;
}
