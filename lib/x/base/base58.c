/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base58.c - Base58 encoding and decoding (Bitcoin alphabet)
 */

#include <x/base/base58.h>

#include <stdlib.h>
#include <string.h>

/* Bitcoin Base58 alphabet */
static const char kAlphabet[] =
  "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Reverse lookup table: ASCII value -> Base58 digit (0-57), -1 for invalid */
static const int8_t kAlphabetMap[128] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  -1, -1, -1, -1, -1, -1, /* 0-9 */
  -1, 9,  10, 11, 12, 13, 14, 15, 16, -1, 17, 18, 19, 20, 21, -1, /* A-O */
  22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, -1, -1, -1, -1, -1, /* P-Z */
  -1, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, -1, 44, 45, 46, /* a-o */
  47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, -1, -1, -1, -1, -1, /* p-z */
};

int xBase58Encode(const uint8_t *src, size_t src_len, char *dst,
                  size_t *dst_len) {
  if (src_len == 0) {
    if (*dst_len < 1) return -1;
    dst[0]   = '\0';
    *dst_len = 0;
    return 0;
  }

  /* Count leading zeros in the input */
  size_t zeros = 0;
  while (zeros < src_len && src[zeros] == 0) {
    zeros++;
  }

  /*
   * Allocate a temporary buffer for the base58 digits.
   * The maximum size is src_len * 138 / 100 + 1.
   */
  size_t   buf_size = src_len * 138 / 100 + 1;
  uint8_t *buf      = (uint8_t *)malloc(buf_size);
  if (!buf) return -1;
  memset(buf, 0, buf_size);

  /* Process each byte of the input */
  for (size_t i = zeros; i < src_len; i++) {
    int carry = src[i];
    for (size_t j = buf_size; j > 0; j--) {
      carry += 256 * buf[j - 1];
      buf[j - 1] = carry % 58;
      carry /= 58;
    }
  }

  /* Skip leading zeros in the base58 result */
  size_t start = 0;
  while (start < buf_size && buf[start] == 0) {
    start++;
  }

  /* Total length: leading '1's + base58 digits */
  size_t encoded_len = zeros + (buf_size - start);

  if (*dst_len < encoded_len + 1) {
    return -1;
  }

  /* Fill leading '1's for each leading zero byte */
  memset(dst, '1', zeros);

  /* Map the remaining digits to the alphabet */
  for (size_t i = start; i < buf_size; i++) {
    dst[zeros + (i - start)] = kAlphabet[buf[i]];
  }

  dst[encoded_len] = '\0';
  *dst_len         = encoded_len;
  free(buf);
  return 0;
}

int xBase58Decode(const char *src, size_t src_len, uint8_t *dst,
                  size_t *dst_len) {
  if (src_len == 0) {
    *dst_len = 0;
    return 0;
  }

  /* Count leading '1's (representing zero bytes) */
  size_t zeros = 0;
  while (zeros < src_len && src[zeros] == '1') {
    zeros++;
  }

  /*
   * Allocate a temporary buffer for the decoded bytes.
   * The maximum size is src_len * 733 / 1000 + 1.
   */
  size_t   buf_size = src_len * 733 / 1000 + 1;
  uint8_t *buf      = (uint8_t *)malloc(buf_size);
  if (!buf) return -1;
  memset(buf, 0, buf_size);

  /* Process each character of the input */
  for (size_t i = zeros; i < src_len; i++) {
    unsigned char ch = (unsigned char)src[i];
    if (ch >= 128 || kAlphabetMap[ch] == -1) {
      return -1; /* Invalid character */
    }
    int carry = kAlphabetMap[ch];
    for (size_t j = buf_size; j > 0; j--) {
      carry += 58 * buf[j - 1];
      buf[j - 1] = carry % 256;
      carry /= 256;
    }
  }

  /* Skip leading zeros in the decoded result */
  size_t start = 0;
  while (start < buf_size && buf[start] == 0) {
    start++;
  }

  /* Total length: leading zero bytes + decoded bytes */
  size_t decoded_len = zeros + (buf_size - start);

  if (*dst_len < decoded_len) {
    return -1;
  }

  /* Fill leading zero bytes */
  memset(dst, 0, zeros);

  /* Copy the remaining decoded bytes */
  memcpy(dst + zeros, buf + start, buf_size - start);

  *dst_len = decoded_len;
  free(buf);
  return 0;
}
