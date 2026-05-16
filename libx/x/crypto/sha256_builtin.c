/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha256_builtin.c - Pure-C SHA-256 implementation (FIPS 180-4)
 *
 * Used as a fallback when neither OpenSSL nor mbedTLS is available.
 */

#include "sha256.h"

#include <string.h>

/* ── Internal state ────────────────────────────────────── */

typedef struct {
  uint32_t state[8];
  uint64_t count;          /**< Total bits processed. */
  uint8_t  buffer[64];
} xSha256Builtin_;

_Static_assert(sizeof(xSha256Builtin_) <= sizeof(((xSha256Ctx *)0)->opaque),
               "xSha256Ctx.opaque too small for builtin backend");

/* ── Helpers ───────────────────────────────────────────── */

static inline uint32_t rotr32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
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

/* SHA-256 functions */
#define Ch(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define Sigma0(x)     (rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22))
#define Sigma1(x)     (rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25))
#define sigma0(x)     (rotr32(x, 7) ^ rotr32(x, 18) ^ ((x) >> 3))
#define sigma1(x)     (rotr32(x, 17) ^ rotr32(x, 19) ^ ((x) >> 10))

/* SHA-256 round constants (first 32 bits of the fractional parts of
   the cube roots of the first 64 primes 2..311). */
static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];

  for (int i = 0; i < 16; i++) {
    w[i] = get_be32(block + i * 4);
  }
  for (int i = 16; i < 64; i++) {
    w[i] = sigma1(w[i - 2]) + w[i - 7] + sigma0(w[i - 15]) + w[i - 16];
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + w[i];
    uint32_t t2 = Sigma0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

/* ── Public API ────────────────────────────────────────── */

xErrno xSha256Init(xSha256Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha256Builtin_ *impl = (xSha256Builtin_ *)ctx->opaque;
  memset(impl, 0, sizeof(*impl));

  /* SHA-256 initial hash values (first 32 bits of the fractional parts
     of the square roots of the first 8 primes 2..19). */
  impl->state[0] = 0x6a09e667;
  impl->state[1] = 0xbb67ae85;
  impl->state[2] = 0x3c6ef372;
  impl->state[3] = 0xa54ff53a;
  impl->state[4] = 0x510e527f;
  impl->state[5] = 0x9b05688c;
  impl->state[6] = 0x1f83d9ab;
  impl->state[7] = 0x5be0cd19;

  return xErrno_Ok;
}

xErrno xSha256Update(xSha256Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha256Builtin_ *impl = (xSha256Builtin_ *)ctx->opaque;
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
    sha256_transform(impl->state, impl->buffer);
    data += space;
    len -= space;
  }

  /* Process full blocks */
  while (len >= 64) {
    sha256_transform(impl->state, data);
    data += 64;
    len -= 64;
  }

  /* Buffer remainder */
  if (len > 0) {
    memcpy(impl->buffer, data, len);
  }

  return xErrno_Ok;
}

xErrno xSha256Final(xSha256Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha256Builtin_ *impl = (xSha256Builtin_ *)ctx->opaque;
  size_t buf_used = (size_t)((impl->count / 8) % 64);

  /* Padding: append 0x80, then zeros, then 8-byte big-endian bit count */
  impl->buffer[buf_used++] = 0x80;

  if (buf_used > 56) {
    memset(impl->buffer + buf_used, 0, 64 - buf_used);
    sha256_transform(impl->state, impl->buffer);
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

  sha256_transform(impl->state, impl->buffer);

  for (int i = 0; i < 8; i++) {
    put_be32(digest + i * 4, impl->state[i]);
  }

  memset(impl, 0, sizeof(*impl));
  return xErrno_Ok;
}

xErrno xSha256(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  xSha256Ctx ctx;
  xErrno err = xSha256Init(&ctx);
  if (err != xErrno_Ok) return err;

  err = xSha256Update(&ctx, data, len);
  if (err != xErrno_Ok) return err;

  return xSha256Final(&ctx, digest);
}
