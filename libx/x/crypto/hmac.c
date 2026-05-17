/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac.c - Generic HMAC implementation (RFC 2104)
 *
 * Works with any hash algorithm described by an xHashVtable.
 * Provides both streaming (init/update/final) and one-shot APIs.
 */

#include "hmac.h"
#include "hash_private.h"
#include "x/base/base.h"

#include <stdlib.h>
#include <string.h>

/* Maximum block size we support (SHA-512 = 128). */
#define HMAC_MAX_BLOCK_SIZE  128
#define HMAC_MAX_DIGEST_SIZE 64
#define HMAC_MAX_CTX_SIZE    256

/* ── Internal layout stored inside xHmacCtx.opaque ──────── */

XDEF_STRUCT(xHmacState_) {
  const xHashVtable *hash;
  uint8_t            opad[HMAC_MAX_BLOCK_SIZE];    /**< Pre-computed opad.          */
  uint8_t            inner_ctx[HMAC_MAX_CTX_SIZE]; /**< Inner hash context buffer. */
};

_Static_assert(sizeof(xHmacState_) <= sizeof(((xHmacCtx *)0)->opaque),
               "xHmacCtx.opaque too small for HMAC state");

/* ── Release resources held by an initialized HMAC state. ── */

static void hmac_cleanup_(xHmacState_ *st) {
  if (!st || !st->hash) return;
  uint8_t discard[HMAC_MAX_DIGEST_SIZE];
  st->hash->final(st->inner_ctx, discard);
  memset(st, 0, sizeof(*st));
}

/* ───────────────────── Streaming API ───────────────────── */

xErrno xHmacInit(xHmacCtx *ctx, const struct xHashVtable *hash, const uint8_t *key,
                 size_t key_len) {
  if (!ctx || !hash || !key) return xErrno_InvalidArg;
  if (hash->block_size > HMAC_MAX_BLOCK_SIZE) return xErrno_InvalidArg;
  if (hash->digest_size > HMAC_MAX_DIGEST_SIZE) return xErrno_InvalidArg;
  if (hash->ctx_size > HMAC_MAX_CTX_SIZE) return xErrno_InvalidArg;

  xHmacState_ *st = (xHmacState_ *)ctx->opaque;
  memset(st, 0, sizeof(*st));
  st->hash = hash;

  const size_t block_size = hash->block_size;
  xErrno       err;

  /* Normalize key: if longer than block size, hash it first. */
  uint8_t k[HMAC_MAX_BLOCK_SIZE];
  memset(k, 0, block_size);

  if (key_len > block_size) {
    err = hash->init(st->inner_ctx);
    if (err != xErrno_Ok) return err;
    err = hash->update(st->inner_ctx, key, key_len);
    if (err != xErrno_Ok) {
      hmac_cleanup_(st);
      return err;
    }
    err = hash->final(st->inner_ctx, k);
    if (err != xErrno_Ok) {
      hmac_cleanup_(st);
      return err;
    }
  } else {
    memcpy(k, key, key_len);
  }

  /* Build ipad and opad */
  uint8_t ipad[HMAC_MAX_BLOCK_SIZE];
  for (size_t i = 0; i < block_size; i++) {
    ipad[i]     = k[i] ^ 0x36;
    st->opad[i] = k[i] ^ 0x5C;
  }

  /* Start inner hash: H(ipad || ...) */
  err = hash->init(st->inner_ctx);
  if (err != xErrno_Ok) return err;

  err = hash->update(st->inner_ctx, ipad, block_size);
  if (err != xErrno_Ok) {
    hmac_cleanup_(st);
    return err;
  }
  return xErrno_Ok;
}

xErrno xHmacUpdate(xHmacCtx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  xHmacState_ *st = (xHmacState_ *)ctx->opaque;
  if (!st->hash) return xErrno_InvalidArg;

  return st->hash->update(st->inner_ctx, data, len);
}

xErrno xHmacFinal(xHmacCtx *ctx, uint8_t *digest) {
  if (!ctx) return xErrno_InvalidArg;

  xHmacState_ *st = (xHmacState_ *)ctx->opaque;
  if (!st->hash) return xErrno_InvalidArg;

  /* If digest is NULL, clean up and return error (avoids leaking
     the hash context allocated by xHmacInit). */
  if (!digest) {
    hmac_cleanup_(st);
    return xErrno_InvalidArg;
  }

  const xHashVtable *hash = st->hash;

  /* Finalize inner hash */
  uint8_t inner_digest[HMAC_MAX_DIGEST_SIZE];
  xErrno  err = hash->final(st->inner_ctx, inner_digest);
  if (err != xErrno_Ok) return err;

  /* Outer hash: H(opad || inner_digest) */
  err = hash->init(st->inner_ctx); /* Reuse the context buffer */
  if (err != xErrno_Ok) return err;
  err = hash->update(st->inner_ctx, st->opad, hash->block_size);
  if (err != xErrno_Ok) {
    hmac_cleanup_(st);
    return err;
  }
  err = hash->update(st->inner_ctx, inner_digest, hash->digest_size);
  if (err != xErrno_Ok) {
    hmac_cleanup_(st);
    return err;
  }

  err = hash->final(st->inner_ctx, digest);

  /* Clear sensitive state */
  memset(st, 0, sizeof(*st));
  return err;
}

/* ───────────────────── One-shot API ───────────────────── */

xErrno xHmac(const struct xHashVtable *hash, const uint8_t *key, size_t key_len,
             const uint8_t *data, size_t data_len, uint8_t *digest) {
  if (!data) return xErrno_InvalidArg;

  xHmacCtx ctx;
  xErrno   err = xHmacInit(&ctx, hash, key, key_len);
  if (err != xErrno_Ok) return err;

  err = xHmacUpdate(&ctx, data, data_len);
  if (err != xErrno_Ok) {
    hmac_cleanup_((xHmacState_ *)ctx.opaque);
    return err;
  }

  err = xHmacFinal(&ctx, digest);
  if (err != xErrno_Ok) {
    hmac_cleanup_((xHmacState_ *)ctx.opaque);
  }
  return err;
}
