/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_crypto_builtin.c - Builtin SHA-1 / Base64 (no external deps)
 *
 * Used when neither OpenSSL nor mbedTLS is available at compile time.
 * SHA-1 implementation follows FIPS 180-4.
 */

#if !defined(X_HAS_OPENSSL) && !defined(X_HAS_MBEDTLS)

#include "ws_crypto.h"

#include <stdint.h>
#include <string.h>

/* ───────────────────── SHA-1 (FIPS 180-4) ───────────────────── */

static uint32_t sha1_rotl(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static void sha1_transform(uint32_t state[5],
                            const unsigned char block[64]) {
  uint32_t w[80];
  uint32_t a, b, c, d, e;

  /* Prepare message schedule */
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) |
           ((uint32_t)block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^
                      w[i - 14] ^ w[i - 16], 1);
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
    e = d;
    d = c;
    c = sha1_rotl(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void xWsSHA1(const unsigned char *input, size_t len,
             unsigned char *output) {
  uint32_t state[5] = {
    0x67452301, 0xEFCDAB89, 0x98BADCFE,
    0x10325476, 0xC3D2E1F0
  };

  /* Process complete 64-byte blocks */
  size_t i;
  for (i = 0; i + 64 <= len; i += 64) {
    sha1_transform(state, input + i);
  }

  /* Final block with padding */
  unsigned char block[64];
  size_t remaining = len - i;
  memcpy(block, input + i, remaining);
  block[remaining++] = 0x80;

  if (remaining > 56) {
    /* Need two blocks for padding */
    memset(block + remaining, 0, 64 - remaining);
    sha1_transform(state, block);
    memset(block, 0, 56);
  } else {
    memset(block + remaining, 0, 56 - remaining);
  }

  /* Append bit length (big-endian 64-bit) */
  uint64_t bits = (uint64_t)len * 8;
  block[56] = (unsigned char)(bits >> 56);
  block[57] = (unsigned char)(bits >> 48);
  block[58] = (unsigned char)(bits >> 40);
  block[59] = (unsigned char)(bits >> 32);
  block[60] = (unsigned char)(bits >> 24);
  block[61] = (unsigned char)(bits >> 16);
  block[62] = (unsigned char)(bits >> 8);
  block[63] = (unsigned char)(bits);
  sha1_transform(state, block);

  /* Output digest (big-endian) */
  for (int j = 0; j < 5; j++) {
    output[j * 4 + 0] = (unsigned char)(state[j] >> 24);
    output[j * 4 + 1] = (unsigned char)(state[j] >> 16);
    output[j * 4 + 2] = (unsigned char)(state[j] >> 8);
    output[j * 4 + 3] = (unsigned char)(state[j]);
  }
}

/* ───────────────────── Base64 ───────────────────── */

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
  "0123456789+/";

int xWsBase64Encode(const unsigned char *input, size_t in_len,
                    char *output, size_t out_len) {
  size_t needed = ((in_len + 2) / 3) * 4 + 1;
  if (out_len < needed) return -1;

  size_t o = 0;
  size_t i = 0;

  while (i + 2 < in_len) {
    uint32_t v = ((uint32_t)input[i] << 16) |
                 ((uint32_t)input[i + 1] << 8) |
                 ((uint32_t)input[i + 2]);
    output[o++] = b64_table[(v >> 18) & 0x3F];
    output[o++] = b64_table[(v >> 12) & 0x3F];
    output[o++] = b64_table[(v >> 6) & 0x3F];
    output[o++] = b64_table[v & 0x3F];
    i += 3;
  }

  if (i < in_len) {
    uint32_t v = (uint32_t)input[i] << 16;
    if (i + 1 < in_len) v |= (uint32_t)input[i + 1] << 8;

    output[o++] = b64_table[(v >> 18) & 0x3F];
    output[o++] = b64_table[(v >> 12) & 0x3F];
    output[o++] = (i + 1 < in_len)
                    ? b64_table[(v >> 6) & 0x3F]
                    : '=';
    output[o++] = '=';
  }

  output[o] = '\0';
  return (int)o;
}

#endif /* !X_HAS_OPENSSL && !X_HAS_MBEDTLS */
