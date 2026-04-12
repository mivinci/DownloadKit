/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha1_mbedtls.c - SHA-1 implementation using mbedTLS
 */

#include "sha1.h"

/*
 * mbedTLS 4.x removed the standalone sha1.h header.
 * Detect at compile time and fall back to the generic MD API.
 */
#if __has_include(<mbedtls/sha1.h>)
#include <mbedtls/sha1.h>
#define XKIT_MBEDTLS_HAS_SHA1_H 1
#else
#include <mbedtls/md.h>
#define XKIT_MBEDTLS_HAS_SHA1_H 0
#endif

#include <string.h>

/* ── Internal layout stored in xSha1Ctx.opaque ─────────── */

#if XKIT_MBEDTLS_HAS_SHA1_H

typedef struct {
  mbedtls_sha1_context mctx;
} xSha1MbedTLS_;

#else /* mbedTLS 4.x: use generic MD context */

typedef struct {
  mbedtls_md_context_t mctx;
} xSha1MbedTLS_;

#endif

_Static_assert(sizeof(xSha1MbedTLS_) <= sizeof(((xSha1Ctx *)0)->opaque),
               "xSha1Ctx.opaque too small for mbedTLS backend");

/* ── Streaming API ─────────────────────────────────────── */

#if XKIT_MBEDTLS_HAS_SHA1_H

xErrno xSha1Init(xSha1Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;
  mbedtls_sha1_init(&impl->mctx);

  if (mbedtls_sha1_starts(&impl->mctx) != 0) {
    mbedtls_sha1_free(&impl->mctx);
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha1Update(xSha1Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;

  if (mbedtls_sha1_update(&impl->mctx, data, len) != 0) {
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha1Final(xSha1Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;

  int ret = mbedtls_sha1_finish(&impl->mctx, digest);
  mbedtls_sha1_free(&impl->mctx);

  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSha1(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  int ret = mbedtls_sha1(data, len, digest);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

#else /* mbedTLS 4.x: generic MD API */

xErrno xSha1Init(xSha1Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;
  mbedtls_md_init(&impl->mctx);

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
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

xErrno xSha1Update(xSha1Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;

  if (mbedtls_md_update(&impl->mctx, data, len) != 0) {
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha1Final(xSha1Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha1MbedTLS_ *impl = (xSha1MbedTLS_ *)ctx->opaque;

  int ret = mbedtls_md_finish(&impl->mctx, digest);
  mbedtls_md_free(&impl->mctx);

  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSha1(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  int ret = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                       data, len, digest);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

#endif /* XKIT_MBEDTLS_HAS_SHA1_H */
