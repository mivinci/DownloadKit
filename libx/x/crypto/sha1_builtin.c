/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha1_builtin.c - Pure-C SHA-1 implementation (RFC 3174)
 *
 * Used as a fallback when neither OpenSSL nor mbedTLS is available.
 */

#include "sha1.h"

#include <string.h>

/* ── Internal state ────────────────────────────────────── */

typedef struct {
  uint32_t state[5];
  uint64_t count;          /**< Total bits processed. */
  uint8_t  buffer[64];
} xSha1Builtin_;

_Static_assert(sizeof(xSha1Builtin_) <= sizeof(((xSha1Ctx *)0)->opaque),
               "xSha1Ctx.opaque too small for builtin backend");

/* ── Helpers ───────────────────────────────────────────── */

static inline uint32_t rotl32(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static inline void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v);
}

static inline uint32_t get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];

  for (int i = 0; i < 16; i++) {
    w[i] = get_be32(block + i * 4);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];

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

    uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

/* ── Public API ────────────────────────────────────────── */

xErrno xSha1Init(xSha1Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha1Builtin_ *impl = (xSha1Builtin_ *)ctx->opaque;
  memset(impl, 0, sizeof(*impl));

  impl->state[0] = 0x67452301;
  impl->state[1] = 0xEFCDAB89;
  impl->state[2] = 0x98BADCFE;
  impl->state[3] = 0x10325476;
  impl->state[4] = 0xC3D2E1F0;

  return xErrno_Ok;
}

xErrno xSha1Update(xSha1Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha1Builtin_ *impl = (xSha1Builtin_ *)ctx->opaque;
  size_t buf_used = (size_t)((impl->count / 8) % 64);

  impl->count += (uint64_t)len * 8;

  /* Fill partial block */
  if (buf_used > 0) {
    size_t space = 64 - buf_used;
    if (len < space) {
      memcpy(impl->buffer + buf_used, data, len);
      return xErrno_Ok;
    }
    memcpy(impl->buffer + buf_used, data, space);
    sha1_transform(impl->state, impl->buffer);
    data += space;
    len -= space;
  }

  /* Process full blocks */
  while (len >= 64) {
    sha1_transform(impl->state, data);
    data += 64;
    len -= 64;
  }

  /* Buffer remainder */
  if (len > 0) {
    memcpy(impl->buffer, data, len);
  }

  return xErrno_Ok;
}

xErrno xSha1Final(xSha1Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha1Builtin_ *impl = (xSha1Builtin_ *)ctx->opaque;
  size_t buf_used = (size_t)((impl->count / 8) % 64);

  /* Padding: append 0x80, then zeros, then 8-byte big-endian bit count */
  impl->buffer[buf_used++] = 0x80;

  if (buf_used > 56) {
    memset(impl->buffer + buf_used, 0, 64 - buf_used);
    sha1_transform(impl->state, impl->buffer);
    buf_used = 0;
  }

  memset(impl->buffer + buf_used, 0, 56 - buf_used);

  /* Append bit count (big-endian) */
  uint64_t bits = impl->count;
  impl->buffer[56] = (uint8_t)(bits >> 56);
  impl->buffer[57] = (uint8_t)(bits >> 48);
  impl->buffer[58] = (uint8_t)(bits >> 40);
  impl->buffer[59] = (uint8_t)(bits >> 32);
  impl->buffer[60] = (uint8_t)(bits >> 24);
  impl->buffer[61] = (uint8_t)(bits >> 16);
  impl->buffer[62] = (uint8_t)(bits >> 8);
  impl->buffer[63] = (uint8_t)(bits);

  sha1_transform(impl->state, impl->buffer);

  for (int i = 0; i < 5; i++) {
    put_be32(digest + i * 4, impl->state[i]);
  }

  memset(impl, 0, sizeof(*impl));
  return xErrno_Ok;
}

xErrno xSha1(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  xSha1Ctx ctx;
  xErrno err = xSha1Init(&ctx);
  if (err != xErrno_Ok) return err;

  err = xSha1Update(&ctx, data, len);
  if (err != xErrno_Ok) return err;

  return xSha1Final(&ctx, digest);
}
