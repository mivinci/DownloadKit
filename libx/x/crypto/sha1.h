/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha1.h - SHA-1 hash interface
 *
 * Provides both a one-shot convenience function and a streaming
 * (init/update/final) API for computing SHA-1 digests.
 *
 * The underlying implementation is selected at build time based on
 * X_TLS_BACKEND (OpenSSL, mbedTLS, or a built-in pure-C fallback).
 */

#ifndef XCRYPTO_SHA1_H
#define XCRYPTO_SHA1_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

#define XCRYPTO_SHA1_DIGEST_SIZE 20 /**< SHA-1 digest length in bytes. */
#define XCRYPTO_SHA1_BLOCK_SIZE  64 /**< SHA-1 internal block size.    */

/* ───────────────────── Streaming API ───────────────────── */

/**
 * @brief Opaque SHA-1 context.
 *
 * Large enough to hold any backend's state without heap allocation.
 */
XDEF_STRUCT(xSha1Ctx) {
  uint8_t opaque[128]; /**< Backend-specific state. */
};

/**
 * @brief Initialize a SHA-1 context.
 *
 * @param ctx  SHA-1 context to initialize.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xSha1Init(xSha1Ctx *ctx);

/**
 * @brief Feed data into the SHA-1 context.
 *
 * May be called multiple times to hash data incrementally.
 *
 * @param ctx   SHA-1 context.
 * @param data  Data to hash.
 * @param len   Length of data in bytes.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xSha1Update(xSha1Ctx *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finalize the hash and produce the digest.
 *
 * After calling this, the context must be re-initialized before reuse.
 *
 * @param ctx     SHA-1 context.
 * @param digest  Output buffer (must be at least XCRYPTO_SHA1_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSha1Final(xSha1Ctx *ctx, uint8_t *digest);

/* ───────────────────── One-shot API ───────────────────── */

/**
 * @brief Compute SHA-1 of a buffer in one call.
 *
 * @param data    Input data.
 * @param len     Length of input data.
 * @param digest  Output buffer (must be at least XCRYPTO_SHA1_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSha1(const uint8_t *data, size_t len, uint8_t *digest);

#endif /* XCRYPTO_SHA1_H */
