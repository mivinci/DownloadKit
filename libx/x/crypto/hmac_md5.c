/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_md5.c - HMAC-MD5 convenience wrapper + MD5 hash vtable
 */

#include "hmac_md5.h"
#include "hash_private.h"
#include "hmac.h"
#include "md5.h"

/* ── MD5 vtable shims (void* → xMd5Ctx*) ───────────────── */

static xErrno md5_vt_init(void *ctx_buf) {
  return xMd5Init((xMd5Ctx *)ctx_buf);
}

static xErrno md5_vt_update(void *ctx_buf, const uint8_t *data, size_t len) {
  return xMd5Update((xMd5Ctx *)ctx_buf, data, len);
}

static xErrno md5_vt_final(void *ctx_buf, uint8_t *digest) {
  return xMd5Final((xMd5Ctx *)ctx_buf, digest);
}

const xHashVtable xHashVtableMd5 = {
  .digest_size = XCRYPTO_MD5_DIGEST_SIZE,
  .block_size  = XCRYPTO_MD5_BLOCK_SIZE,
  .ctx_size    = sizeof(xMd5Ctx),
  .init        = md5_vt_init,
  .update      = md5_vt_update,
  .final       = md5_vt_final,
};

/* ── Convenience wrapper ────────────────────────────────── */

xErrno xHmacMd5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                uint8_t *digest) {
  return xHmac(&xHashVtableMd5, key, key_len, data, data_len, digest);
}
