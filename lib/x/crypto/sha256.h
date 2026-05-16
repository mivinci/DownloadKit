/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha256.h - SHA-256 hash interface
 *
 * Provides both a one-shot convenience function and a streaming
 * (init/update/final) API for computing SHA-256 digests.
 *
 * The underlying implementation is selected at build time based on
 * MOO_TLS_BACKEND (OpenSSL, mbedTLS, or a built-in pure-C fallback).
 */

#ifndef XCRYPTO_SHA256_H
#define XCRYPTO_SHA256_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

#define XCRYPTO_SHA256_DIGEST_SIZE 32 /**< SHA-256 digest length in bytes. */
#define XCRYPTO_SHA256_BLOCK_SIZE  64 /**< SHA-256 internal block size.    */

/* ───────────────────── Streaming API ───────────────────── */

/**
 * @brief Opaque SHA-256 context.
 *
 * Large enough to hold any backend's state without heap allocation.
 */
XDEF_STRUCT(xSha256Ctx) {
  uint8_t opaque[128]; /**< Backend-specific state. */
};

/**
 * @brief Initialize a SHA-256 context.
 *
 * @param ctx  SHA-256 context to initialize.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xSha256Init(xSha256Ctx *ctx);

/**
 * @brief Feed data into the SHA-256 context.
 *
 * May be called multiple times to hash data incrementally.
 *
 * @param ctx   SHA-256 context.
 * @param data  Data to hash.
 * @param len   Length of data in bytes.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xSha256Update(xSha256Ctx *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finalize the hash and produce the digest.
 *
 * After calling this, the context must be re-initialized before reuse.
 *
 * @param ctx     SHA-256 context.
 * @param digest  Output buffer (must be at least XCRYPTO_SHA256_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSha256Final(xSha256Ctx *ctx, uint8_t *digest);

/* ───────────────────── One-shot API ───────────────────── */

/**
 * @brief Compute SHA-256 of a buffer in one call.
 *
 * @param data    Input data.
 * @param len     Length of input data.
 * @param digest  Output buffer (must be at least XCRYPTO_SHA256_DIGEST_SIZE).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSha256(const uint8_t *data, size_t len, uint8_t *digest);

#endif /* XCRYPTO_SHA256_H */
