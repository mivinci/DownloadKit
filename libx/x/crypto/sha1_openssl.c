/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha1_openssl.c - SHA-1 implementation using OpenSSL EVP
 */

#include "sha1.h"

#include <openssl/evp.h>

#include <string.h>

/* ── Internal layout stored in xSha1Ctx.opaque ─────────── */

typedef struct {
  EVP_MD_CTX *mdctx;
} xSha1OpenSSL_;

_Static_assert(sizeof(xSha1OpenSSL_) <= sizeof(((xSha1Ctx *)0)->opaque),
               "xSha1Ctx.opaque too small for OpenSSL backend");

xErrno xSha1Init(xSha1Ctx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  xSha1OpenSSL_ *impl = (xSha1OpenSSL_ *)ctx->opaque;
  memset(impl, 0, sizeof(*impl));

  impl->mdctx = EVP_MD_CTX_new();
  if (!impl->mdctx) return xErrno_NoMemory;

  if (EVP_DigestInit_ex(impl->mdctx, EVP_sha1(), NULL) != 1) {
    EVP_MD_CTX_free(impl->mdctx);
    impl->mdctx = NULL;
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha1Update(xSha1Ctx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xSha1OpenSSL_ *impl = (xSha1OpenSSL_ *)ctx->opaque;
  if (!impl->mdctx) return xErrno_InvalidState;

  if (EVP_DigestUpdate(impl->mdctx, data, len) != 1) {
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

xErrno xSha1Final(xSha1Ctx *ctx, uint8_t *digest) {
  if (!ctx || !digest) return xErrno_InvalidArg;

  xSha1OpenSSL_ *impl = (xSha1OpenSSL_ *)ctx->opaque;
  if (!impl->mdctx) return xErrno_InvalidState;

  unsigned int len = 0;
  int ok = EVP_DigestFinal_ex(impl->mdctx, digest, &len);

  EVP_MD_CTX_free(impl->mdctx);
  impl->mdctx = NULL;

  return (ok == 1) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSha1(const uint8_t *data, size_t len, uint8_t *digest) {
  if (!data || !digest) return xErrno_InvalidArg;

  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  if (!mdctx) return xErrno_NoMemory;

  int ok = EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL) &&
           EVP_DigestUpdate(mdctx, data, len) &&
           EVP_DigestFinal_ex(mdctx, digest, NULL);

  EVP_MD_CTX_free(mdctx);
  return ok ? xErrno_Ok : xErrno_SysError;
}
