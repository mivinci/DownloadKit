/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md5.h - MD5 hash interface (RFC 1321)
 *
 * Provides both a one-shot convenience function and a streaming
 * (init/update/final) API for computing MD5 digests.
 * Pure-C implementation, no external dependencies required.
 */

#ifndef XCRYPTO_MD5_H
#define XCRYPTO_MD5_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

#define XCRYPTO_MD5_DIGEST_SIZE 16 /**< MD5 digest length in bytes. */
#define XCRYPTO_MD5_BLOCK_SIZE  64 /**< MD5 internal block size.    */

/* ───────────────────── Streaming API ───────────────────── */

/**
 * @brief Opaque MD5 context.
 *
 * Large enough to hold the internal state without heap allocation.
 */
XDEF_STRUCT(xMd5Ctx) {
  uint8_t opaque[128]; /**< Internal state. */
};

/**
 * @brief Initialize a MD5 context.
 *
 * @param ctx  MD5 context to initialize.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xMd5Init(xMd5Ctx *ctx);

/**
 * @brief Feed data into the MD5 context.
 *
 * May be called multiple times to hash data incrementally.
 *
 * @param ctx   MD5 context.
 * @param data  Data to hash.
 * @param len   Length of data in bytes.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xMd5Update(xMd5Ctx *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finalize the hash and produce the digest.
 *
 * After calling this, the context must be re-initialized before reuse.
 *
 * @param ctx     MD5 context.
 * @param digest  Output buffer (must be at least XCRYPTO_MD5_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xMd5Final(xMd5Ctx *ctx, uint8_t *digest);

/* ───────────────────── One-shot API ───────────────────── */

/**
 * @brief Compute MD5 of a buffer in one call.
 *
 * @param data    Input data.
 * @param len     Length of input data.
 * @param digest  Output buffer (must be at least XCRYPTO_MD5_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xMd5(const uint8_t *data, size_t len, uint8_t *digest);

#endif /* XCRYPTO_MD5_H */
