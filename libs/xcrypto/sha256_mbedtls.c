/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha256_mbedtls.c - SHA-256 implementation using mbedTLS
 *
 * Supports three mbedTLS generations:
 *   - 2.x : sha256.h present, uses _ret suffix functions (returns int)
 *   - 3.x : sha256.h present, non-deprecated functions (returns int)
 *   - 4.x : sha256.h removed, fall back to generic MD API
 */

#include "sha256.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif

/*
 * mbedTLS 4.x removed the standalone sha256.h header.
 * Detect at compile time and fall back to the generic MD API.
 */
#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#define MOO_MBEDTLS_HAS_SHA256_H 1
#else
#include <mbedtls/md.h>
#define MOO_MBEDTLS_HAS_SHA256_H 0
#endif

#include <string.h>

/* ── Internal layout stored in xSha256Ctx.opaque ─────────── */

#if MOO_MBEDTLS_HAS_SHA256_H

typedef struct {
  mbedtls_sha256_context mctx;
} xSha256MbedTLS_;

#else /* mbedTLS 4.x: use generic MD context */

typedef struct {
  mbedtls_md_context_t mctx;
} xSha256MbedTLS_;

#endif

_Static_assert(sizeof(xSha256MbedTLS_) <= sizeof(((xSha256Ctx *)0)->opaque),
               "xSha256Ctx.opaque too small for mbedTLS backend");

/* ── Streaming API ─────────────────────────────────────── */

#if MOO_MBEDTLS_HAS_SHA256_H
/* mbedTLS 2.x / 3.x: standalone SHA-256 context */

xErrno xSha256Init(xSha256Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;
  mbedtls_sha256_init(&impl->mctx);

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  /* mbedTLS 3.x: returns int, second arg 0 = SHA-256 (not SHA-224) */
  if (mbedtls_sha256_starts(&impl->mctx, 0) != 0) {
    mbedtls_sha256_free(&impl->mctx);
    return xErrno_SysError;
  }
#else
  /* mbedTLS 2.x: use _ret variant, second arg 0 = SHA-256 */
  if (mbedtls_sha256_starts_ret(&impl->mctx, 0) != 0) {
    mbedtls_sha256_free(&impl->mctx);
    return xErrno_SysError;
  }
#endif

  return xErrno_Ok;
}

xErrno xSha256Update(xSha256Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  if (mbedtls_sha256_update(&impl->mctx, data, len) != 0) {
    return xErrno_SysError;
  }
#else
  if (mbedtls_sha256_update_ret(&impl->mctx, data, len) != 0) {
    return xErrno_SysError;
  }
#endif

  return xErrno_Ok;
}

xErrno xSha256Final(xSha256Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  int ret = mbedtls_sha256_finish(&impl->mctx, digest);
#else
  int ret = mbedtls_sha256_finish_ret(&impl->mctx, digest);
#endif
  mbedtls_sha256_free(&impl->mctx);

  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSha256(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  /* Second arg 0 = SHA-256 (not SHA-224) */
  int ret = mbedtls_sha256(data, len, digest, 0);
#else
  int ret = mbedtls_sha256_ret(data, len, digest, 0);
#endif
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

#else /* mbedTLS 4.x: generic MD API */

xErrno xSha256Init(xSha256Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;
  mbedtls_md_init(&impl->mctx);

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == NULL) return xErrno_SysError;

  if (mbedtls_md_setup(&impl->mctx, info, 0) != 0) {
    mbedtls_md_free(&impl->mctx);
    return xErrno_SysError;
  }

  if (mbedtls_md_starts(&impl->mctx) != 0) {
    mbedtls_md_free(&impl->mctx);
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha256Update(xSha256Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;

  if (mbedtls_md_update(&impl->mctx, data, len) != 0) {
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha256Final(xSha256Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha256MbedTLS_ *impl = (xSha256MbedTLS_ *)ctx->opaque;

  int ret = mbedtls_md_finish(&impl->mctx, digest);
  mbedtls_md_free(&impl->mctx);

  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSha256(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  int ret = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       data, len, digest);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

#endif /* MOO_MBEDTLS_HAS_SHA256_H */
