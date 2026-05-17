/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md5.c - Pure-C MD5 implementation (RFC 1321)
 *
 * Provides both streaming (init/update/final) and one-shot APIs.
 * No external dependencies required.
 */

#include "md5.h"

#include <string.h>

/* ── Internal state ────────────────────────────────────── */

typedef struct {
  uint32_t state[4];
  uint64_t count; /**< Total bytes processed. */
  uint8_t  buffer[64];
} xMd5Builtin_;

_Static_assert(sizeof(xMd5Builtin_) <= sizeof(((xMd5Ctx *)0)->opaque),
               "xMd5Ctx.opaque too small for MD5 state");

/* ───────────────────── Helpers ───────────────────── */

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
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const int md5_s[64] = {
  7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
  4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t M[16];
  for (int i = 0; i < 16; i++) {
    M[i] = (uint32_t)block[i * 4 + 0] | ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
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

/* ───────────────────── Streaming API ───────────────────── */

xErrno xMd5Init(xMd5Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xMd5Builtin_ *impl = (xMd5Builtin_ *)ctx->opaque;
  memset(impl, 0, sizeof(*impl));

  impl->state[0] = 0x67452301;
  impl->state[1] = 0xefcdab89;
  impl->state[2] = 0x98badcfe;
  impl->state[3] = 0x10325476;

  return xErrno_Ok;
}

xErrno xMd5Update(xMd5Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xMd5Builtin_ *impl     = (xMd5Builtin_ *)ctx->opaque;
  size_t        buf_used = (size_t)(impl->count % 64);

  impl->count += len;

  /* Fill partial block */
  if (buf_used > 0) {
    size_t space = 64 - buf_used;
    if (len < space) {
      memcpy(impl->buffer + buf_used, data, len);
      return xErrno_Ok;
    }
    memcpy(impl->buffer + buf_used, data, space);
    md5_transform(impl->state, impl->buffer);
    data += space;
    len -= space;
  }

  /* Process full blocks */
  while (len >= 64) {
    md5_transform(impl->state, data);
    data += 64;
    len -= 64;
  }

  /* Buffer remainder */
  if (len > 0) {
    memcpy(impl->buffer, data, len);
  }

  return xErrno_Ok;
}

xErrno xMd5Final(xMd5Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xMd5Builtin_ *impl     = (xMd5Builtin_ *)ctx->opaque;
  size_t        buf_used = (size_t)(impl->count % 64);

  /* Padding: append 0x80, then zeros, then 8-byte little-endian bit count */
  impl->buffer[buf_used++] = 0x80;

  if (buf_used > 56) {
    memset(impl->buffer + buf_used, 0, 64 - buf_used);
    md5_transform(impl->state, impl->buffer);
    buf_used = 0;
  }

  memset(impl->buffer + buf_used, 0, 56 - buf_used);

  /* Append bit count (little-endian) */
  uint64_t bits    = impl->count * 8;
  impl->buffer[56] = (uint8_t)(bits);
  impl->buffer[57] = (uint8_t)(bits >> 8);
  impl->buffer[58] = (uint8_t)(bits >> 16);
  impl->buffer[59] = (uint8_t)(bits >> 24);
  impl->buffer[60] = (uint8_t)(bits >> 32);
  impl->buffer[61] = (uint8_t)(bits >> 40);
  impl->buffer[62] = (uint8_t)(bits >> 48);
  impl->buffer[63] = (uint8_t)(bits >> 56);

  md5_transform(impl->state, impl->buffer);

  /* Output in little-endian */
  for (int j = 0; j < 4; j++) {
    digest[j * 4 + 0] = (uint8_t)(impl->state[j]);
    digest[j * 4 + 1] = (uint8_t)(impl->state[j] >> 8);
    digest[j * 4 + 2] = (uint8_t)(impl->state[j] >> 16);
    digest[j * 4 + 3] = (uint8_t)(impl->state[j] >> 24);
  }

  memset(impl, 0, sizeof(*impl));
  return xErrno_Ok;
}

/* ───────────────────── One-shot API ───────────────────── */

xErrno xMd5(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  xMd5Ctx ctx;
  xErrno  err = xMd5Init(&ctx);
  if (err != xErrno_Ok) return err;

  err = xMd5Update(&ctx, data, len);
  if (err != xErrno_Ok) return err;

  return xMd5Final(&ctx, digest);
}
