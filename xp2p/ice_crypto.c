/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_crypto.c - Builtin SHA-1, HMAC-SHA1, and CRC-32
 *
 * SHA-1 follows FIPS 180-4. HMAC-SHA1 follows RFC 2104.
 * CRC-32 uses the standard polynomial 0xEDB88320 (reflected).
 */

#include "ice_crypto.h"

#include <stdlib.h>
#include <string.h>

/* ───────────────────── SHA-1 (FIPS 180-4) ───────────────────── */

static uint32_t sha1_rotl(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  uint32_t a, b, c, d, e;

  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];

  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }

    uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
    e             = d;
    d             = c;
    c             = sha1_rotl(b, 30);
    b             = a;
    a             = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void xIceSHA1(const uint8_t *input, size_t len, uint8_t *output) {
  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                       0xC3D2E1F0};

  size_t i;
  for (i = 0; i + 64 <= len; i += 64) {
    sha1_transform(state, input + i);
  }

  uint8_t block[64];
  size_t  remaining = len - i;
  memcpy(block, input + i, remaining);
  block[remaining++] = 0x80;

  if (remaining > 56) {
    memset(block + remaining, 0, 64 - remaining);
    sha1_transform(state, block);
    memset(block, 0, 56);
  } else {
    memset(block + remaining, 0, 56 - remaining);
  }

  uint64_t bits = (uint64_t)len * 8;
  block[56]     = (uint8_t)(bits >> 56);
  block[57]     = (uint8_t)(bits >> 48);
  block[58]     = (uint8_t)(bits >> 40);
  block[59]     = (uint8_t)(bits >> 32);
  block[60]     = (uint8_t)(bits >> 24);
  block[61]     = (uint8_t)(bits >> 16);
  block[62]     = (uint8_t)(bits >> 8);
  block[63]     = (uint8_t)(bits);
  sha1_transform(state, block);

  for (int j = 0; j < 5; j++) {
    output[j * 4 + 0] = (uint8_t)(state[j] >> 24);
    output[j * 4 + 1] = (uint8_t)(state[j] >> 16);
    output[j * 4 + 2] = (uint8_t)(state[j] >> 8);
    output[j * 4 + 3] = (uint8_t)(state[j]);
  }
}

/* ───────────────────── HMAC-SHA1 (RFC 2104) ───────────────────── */

void xIceHmacSHA1(const uint8_t *key, size_t key_len, const uint8_t *data,
                  size_t data_len, uint8_t *output) {
  uint8_t k[XICE_SHA1_BLOCK_SIZE];
  uint8_t ipad[XICE_SHA1_BLOCK_SIZE];
  uint8_t opad[XICE_SHA1_BLOCK_SIZE];

  /* If key is longer than block size, hash it first */
  if (key_len > XICE_SHA1_BLOCK_SIZE) {
    xIceSHA1(key, key_len, k);
    memset(k + XICE_SHA1_DIGEST_SIZE, 0,
           XICE_SHA1_BLOCK_SIZE - XICE_SHA1_DIGEST_SIZE);
  } else {
    memcpy(k, key, key_len);
    memset(k + key_len, 0, XICE_SHA1_BLOCK_SIZE - key_len);
  }

  /* ipad = key XOR 0x36, opad = key XOR 0x5C */
  for (int i = 0; i < XICE_SHA1_BLOCK_SIZE; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5C;
  }

  /* inner hash: SHA1(ipad || data) */
  size_t   inner_len = XICE_SHA1_BLOCK_SIZE + data_len;
  uint8_t *inner_buf = (uint8_t *)malloc(inner_len);
  if (!inner_buf) {
    memset(output, 0, XICE_SHA1_DIGEST_SIZE);
    return;
  }
  memcpy(inner_buf, ipad, XICE_SHA1_BLOCK_SIZE);
  memcpy(inner_buf + XICE_SHA1_BLOCK_SIZE, data, data_len);

  uint8_t inner_hash[XICE_SHA1_DIGEST_SIZE];
  xIceSHA1(inner_buf, inner_len, inner_hash);
  free(inner_buf);

  /* outer hash: SHA1(opad || inner_hash) */
  uint8_t outer_buf[XICE_SHA1_BLOCK_SIZE + XICE_SHA1_DIGEST_SIZE];
  memcpy(outer_buf, opad, XICE_SHA1_BLOCK_SIZE);
  memcpy(outer_buf + XICE_SHA1_BLOCK_SIZE, inner_hash, XICE_SHA1_DIGEST_SIZE);

  xIceSHA1(outer_buf, XICE_SHA1_BLOCK_SIZE + XICE_SHA1_DIGEST_SIZE, output);
}

/* ───────────────────── CRC-32 ───────────────────── */

/**
 * CRC-32 lookup table (polynomial 0xEDB88320, reflected).
 * Generated at compile time for all 256 byte values.
 */
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

uint32_t xIceCrc32(const uint8_t *data, size_t len) {
  crc32_init_table();
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

/* ───────────────────── MD5 (RFC 1321) ───────────────────── */

static uint32_t md5_F(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) | (~x & z);
}
static uint32_t md5_G(uint32_t x, uint32_t y, uint32_t z) {
  return (x & z) | (y & ~z);
}
static uint32_t md5_H(uint32_t x, uint32_t y, uint32_t z) {
  return x ^ y ^ z;
}
static uint32_t md5_I(uint32_t x, uint32_t y, uint32_t z) {
  return y ^ (x | ~z);
}

static uint32_t md5_rotl(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static const uint32_t md5_T[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
  0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
  0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
  0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
  0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
  0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
  0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
  0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const int md5_s[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t M[16];
  for (int i = 0; i < 16; i++) {
    M[i] = (uint32_t)block[i * 4 + 0] | ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) |
           ((uint32_t)block[i * 4 + 3] << 24);
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int      g;
    if (i < 16) {
      f = md5_F(b, c, d);
      g = i;
    } else if (i < 32) {
      f = md5_G(b, c, d);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = md5_H(b, c, d);
      g = (3 * i + 5) % 16;
    } else {
      f = md5_I(b, c, d);
      g = (7 * i) % 16;
    }

    uint32_t temp = d;
    d             = c;
    c             = b;
    b             = b + md5_rotl(a + f + md5_T[i] + M[g], md5_s[i]);
    a             = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

void xIceMD5(const uint8_t *input, size_t len, uint8_t *output) {
  uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

  size_t i;
  for (i = 0; i + 64 <= len; i += 64) {
    md5_transform(state, input + i);
  }

  uint8_t block[64];
  size_t  remaining = len - i;
  memcpy(block, input + i, remaining);
  block[remaining++] = 0x80;

  if (remaining > 56) {
    memset(block + remaining, 0, 64 - remaining);
    md5_transform(state, block);
    memset(block, 0, 56);
  } else {
    memset(block + remaining, 0, 56 - remaining);
  }

  /* Length in bits, little-endian */
  uint64_t bits = (uint64_t)len * 8;
  block[56]     = (uint8_t)(bits);
  block[57]     = (uint8_t)(bits >> 8);
  block[58]     = (uint8_t)(bits >> 16);
  block[59]     = (uint8_t)(bits >> 24);
  block[60]     = (uint8_t)(bits >> 32);
  block[61]     = (uint8_t)(bits >> 40);
  block[62]     = (uint8_t)(bits >> 48);
  block[63]     = (uint8_t)(bits >> 56);
  md5_transform(state, block);

  /* Output in little-endian */
  for (int j = 0; j < 4; j++) {
    output[j * 4 + 0] = (uint8_t)(state[j]);
    output[j * 4 + 1] = (uint8_t)(state[j] >> 8);
    output[j * 4 + 2] = (uint8_t)(state[j] >> 16);
    output[j * 4 + 3] = (uint8_t)(state[j] >> 24);
  }
}