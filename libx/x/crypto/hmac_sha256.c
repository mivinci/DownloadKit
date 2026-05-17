/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_sha256.c - HMAC-SHA256 convenience wrapper + SHA-256 hash vtable
 */

#include "hmac_sha256.h"
#include "hash_private.h"
#include "hmac.h"
#include "sha256.h"

/* ── SHA-256 vtable shims (void* → xSha256Ctx*) ────────── */

static xErrno sha256_vt_init(void *ctx_buf) {
  return xSha256Init((xSha256Ctx *)ctx_buf);
}

static xErrno sha256_vt_update(void *ctx_buf, const uint8_t *data, size_t len) {
  return xSha256Update((xSha256Ctx *)ctx_buf, data, len);
}

static xErrno sha256_vt_final(void *ctx_buf, uint8_t *digest) {
  return xSha256Final((xSha256Ctx *)ctx_buf, digest);
}

const xHashVtable xHashVtableSha256 = {
  .digest_size = XCRYPTO_SHA256_DIGEST_SIZE,
  .block_size  = XCRYPTO_SHA256_BLOCK_SIZE,
  .ctx_size    = sizeof(xSha256Ctx),
  .init        = sha256_vt_init,
  .update      = sha256_vt_update,
  .final       = sha256_vt_final,
};

/* ── Convenience wrapper ────────────────────────────────── */

xErrno xHmacSha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                   uint8_t *digest) {
  return xHmac(&xHashVtableSha256, key, key_len, data, data_len, digest);
}
