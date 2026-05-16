/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac.h - Generic HMAC interface (RFC 2104)
 *
 * Provides a generic HMAC function that works with any hash algorithm
 * described by an xHashVtable, plus convenience wrappers for common
 * hash algorithms (SHA-1, MD5).
 */

#ifndef XCRYPTO_HMAC_H
#define XCRYPTO_HMAC_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

/* Forward declaration — full definition in hash_private.h */
struct xHashVtable;

/* ───────────────────── Streaming API ───────────────────── */

/**
 * @brief Opaque HMAC context for incremental computation.
 *
 * Holds the hash vtable pointer, the pre-computed opad, and the
 * inner hash context. Large enough for any supported hash algorithm.
 */
XDEF_STRUCT(xHmacCtx) {
  uint8_t opaque[512]; /**< Internal state — do not access directly. */
};

/**
 * @brief Initialize an HMAC context with the given hash and key.
 *
 * After this call the inner hash is primed with (key ^ ipad).
 * Call xHmacUpdate() to feed data, then xHmacFinal() to finish.
 *
 * @param ctx      HMAC context to initialize.
 * @param hash     Hash algorithm vtable (e.g. &xHashVtableSha1).
 * @param key      HMAC key.
 * @param key_len  Length of key in bytes.
 * @return         xErrno_Ok on success.
 */
XCAPI(xErrno) xHmacInit(xHmacCtx *ctx,
                         const struct xHashVtable *hash,
                         const uint8_t *key, size_t key_len);

/**
 * @brief Feed data into the HMAC context.
 *
 * May be called multiple times to hash data incrementally.
 *
 * @param ctx   HMAC context.
 * @param data  Data to hash.
 * @param len   Length of data in bytes.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xHmacUpdate(xHmacCtx *ctx,
                           const uint8_t *data, size_t len);

/**
 * @brief Finalize the HMAC and produce the digest.
 *
 * Completes the inner hash, then computes the outer hash
 * H(opad || inner_digest). After this call the context must be
 * re-initialized before reuse.
 *
 * @param ctx     HMAC context.
 * @param digest  Output buffer (must be at least hash->digest_size).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xHmacFinal(xHmacCtx *ctx, uint8_t *digest);

/* ───────────────────── One-shot API ───────────────────── */

/**
 * @brief Compute HMAC of a buffer using the given hash algorithm.
 *
 * @param hash     Hash algorithm vtable (e.g. &xHashVtableSha1).
 * @param key      HMAC key.
 * @param key_len  Length of key in bytes.
 * @param data     Input data.
 * @param data_len Length of data in bytes.
 * @param digest   Output buffer (must be at least hash->digest_size).
 * @return         xErrno_Ok on success.
 */
XCAPI(xErrno) xHmac(const struct xHashVtable *hash,
                     const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t *digest);

#endif /* XCRYPTO_HMAC_H */
