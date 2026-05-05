/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_sha1.c - HMAC-SHA1 convenience wrapper + SHA-1 hash vtable
 */

#include "hmac_sha1.h"
#include "hmac.h"
#include "hash_private.h"
#include "sha1.h"

/* ── SHA-1 vtable shims (void* → xSha1Ctx*) ────────────── */

static xErrno sha1_vt_init(void *ctx_buf) {
  return xSha1Init((xSha1Ctx *)ctx_buf);
}

static xErrno sha1_vt_update(void *ctx_buf, const uint8_t *data, size_t len) {
  return xSha1Update((xSha1Ctx *)ctx_buf, data, len);
}

static xErrno sha1_vt_final(void *ctx_buf, uint8_t *digest) {
  return xSha1Final((xSha1Ctx *)ctx_buf, digest);
}

const xHashVtable xHashVtableSha1 = {
  .digest_size = XCRYPTO_SHA1_DIGEST_SIZE,
  .block_size  = XCRYPTO_SHA1_BLOCK_SIZE,
  .ctx_size    = sizeof(xSha1Ctx),
  .init        = sha1_vt_init,
  .update      = sha1_vt_update,
  .final       = sha1_vt_final,
};

/* ── Convenience wrapper ────────────────────────────────── */

xErrno xHmacSha1(const uint8_t *key, size_t key_len,
                  const uint8_t *data, size_t data_len,
                  uint8_t *digest) {
  return xHmac(&xHashVtableSha1, key, key_len, data, data_len, digest);
}
