/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base64.c - Base64 encoding and decoding (RFC 4648)
 */

#include <x/base/base64.h>

#include <string.h>

static const char kAlphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Map a base64 character (standard or URL-safe) to its 6-bit value.
 * Returns -1 for invalid input.  Padding '=' is NOT handled here;
 * the caller must skip padding before calling this function. */
static int base64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62; /* standard or URL-safe */
  if (c == '/' || c == '_') return 63; /* standard or URL-safe */
  return -1;
}

int xBase64Encode(const uint8_t *src, size_t src_len, char *dst,
                  size_t *dst_len) {
  if (!dst || !dst_len) return -1;
  if (!src && src_len > 0) return -1;

  size_t need = ((src_len + 2) / 3) * 4;
  if (*dst_len < need + 1) return -1;

  if (src_len == 0) {
    dst[0]   = '\0';
    *dst_len = 0;
    return 0;
  }

  size_t out = 0;
  size_t i   = 0;
  for (; i + 3 <= src_len; i += 3) {
    dst[out++] = kAlphabet[src[i] >> 2];
    dst[out++] = kAlphabet[((src[i] & 0x03) << 4) | ((src[i + 1] >> 4) & 0x0F)];
    dst[out++] =
      kAlphabet[((src[i + 1] & 0x0F) << 2) | ((src[i + 2] >> 6) & 0x03)];
    dst[out++] = kAlphabet[src[i + 2] & 0x3F];
  }

  size_t remain = src_len - i;
  if (remain == 1) {
    dst[out++] = kAlphabet[src[i] >> 2];
    dst[out++] = kAlphabet[(src[i] & 0x03) << 4];
    dst[out++] = '=';
    dst[out++] = '=';
  } else if (remain == 2) {
    dst[out++] = kAlphabet[src[i] >> 2];
    dst[out++] = kAlphabet[((src[i] & 0x03) << 4) | ((src[i + 1] >> 4) & 0x0F)];
    dst[out++] = kAlphabet[(src[i + 1] & 0x0F) << 2];
    dst[out++] = '=';
  }

  dst[out] = '\0';
  *dst_len = out;
  return 0;
}

int xBase64Decode(const char *src, size_t src_len, uint8_t *dst,
                  size_t *dst_len) {
  if (!src || !dst || !dst_len) return -1;
  if (src_len == 0) {
    *dst_len = 0;
    return 0;
  }

  /* Validate characters and locate padding.
   * Padding '=' may only appear at the end. */
  size_t pad_count = 0;
  int    got_pad   = 0;
  for (size_t i = 0; i < src_len; i++) {
    if (src[i] == '=') {
      got_pad = 1;
      pad_count++;
    } else {
      if (got_pad) return -1; /* padding must be contiguous at end */
      if (base64_val(src[i]) < 0) return -1;
    }
  }
  if (pad_count > 2) return -1;

  /* Compute output byte count.
   * content_len * 6 bits / 8 = content_len * 3 / 4 bytes. */
  size_t content_len = src_len - pad_count;
  size_t need        = (content_len * 3) / 4;
  /* Handle case where content_len % 4 == 1 (invalid). */
  if (content_len % 4 == 1) return -1;

  if (*dst_len < need) return -1;

  /* Decode: collect 4 sextets at a time, emit up to 3 bytes. */
  size_t out = 0;
  size_t i   = 0;
  while (i < content_len) {
    int v0 = base64_val(src[i++]);
    int v1 = (i < content_len) ? base64_val(src[i++]) : 0;
    int v2 = (i < content_len) ? base64_val(src[i++]) : 0;
    int v3 = (i < content_len) ? base64_val(src[i++]) : 0;

    dst[out++] = (uint8_t)((v0 << 2) | (v1 >> 4));
    if (out < need) {
      dst[out++] = (uint8_t)(((v1 & 0x0F) << 4) | (v2 >> 2));
    }
    if (out < need) {
      dst[out++] = (uint8_t)(((v2 & 0x03) << 6) | v3);
    }
  }

  *dst_len = out;
  return 0;
}
