/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hash_private.h - Internal hash algorithm vtable
 *
 * Defines a generic hash algorithm descriptor used by the HMAC
 * implementation. Each hash backend provides a static instance.
 */

#ifndef XCRYPTO_HASH_PRIVATE_H
#define XCRYPTO_HASH_PRIVATE_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Hash algorithm vtable
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Virtual dispatch table for hash algorithms.
 *
 * Each hash algorithm (SHA-1, MD5, ...) provides a static instance of
 * this struct. The generic HMAC implementation dispatches through these
 * function pointers.
 *
 * @note ctx_buf is a caller-provided buffer of at least @c ctx_size
 *       bytes. The init/update/final functions cast it to their
 *       internal context type.
 */
XDEF_STRUCT(xHashVtable) {
  size_t  digest_size; /**< Output digest length in bytes.      */
  size_t  block_size;  /**< Internal block size in bytes.       */
  size_t  ctx_size;    /**< Size of the opaque context struct.  */

  xErrno (*init)(void *ctx_buf);
  xErrno (*update)(void *ctx_buf, const uint8_t *data, size_t len);
  xErrno (*final)(void *ctx_buf, uint8_t *digest);
};

#ifdef __cplusplus
}
#endif

#endif /* XCRYPTO_HASH_PRIVATE_H */
