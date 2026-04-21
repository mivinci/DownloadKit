/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_sha1.h - HMAC-SHA1 convenience wrapper
 */

#ifndef XCRYPTO_HMAC_SHA1_H
#define XCRYPTO_HMAC_SHA1_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stddef.h>
#include <stdint.h>

#include "hash_private.h"

/** @brief SHA-1 hash vtable (backend selected at build time). */
extern const xHashVtable xHashVtableSha1;

/**
 * @brief Compute HMAC-SHA1 of a buffer in one call.
 *
 * @param key      HMAC key.
 * @param key_len  Length of key in bytes.
 * @param data     Input data.
 * @param data_len Length of data in bytes.
 * @param digest   Output buffer (must be at least 20 bytes).
 * @return         xErrno_Ok on success.
 */
XCAPI(xErrno) xHmacSha1(const uint8_t *key, size_t key_len,
                         const uint8_t *data, size_t data_len,
                         uint8_t *digest);

#endif /* XCRYPTO_HMAC_SHA1_H */
