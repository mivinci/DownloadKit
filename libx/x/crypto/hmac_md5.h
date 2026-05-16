/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_md5.h - HMAC-MD5 convenience wrapper
 */

#ifndef XCRYPTO_HMAC_MD5_H
#define XCRYPTO_HMAC_MD5_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

#include "hash_private.h"

/** @brief MD5 hash vtable (pure-C implementation). */
extern const xHashVtable xHashVtableMd5;

/**
 * @brief Compute HMAC-MD5 of a buffer in one call.
 *
 * @param key      HMAC key.
 * @param key_len  Length of key in bytes.
 * @param data     Input data.
 * @param data_len Length of data in bytes.
 * @param digest   Output buffer (must be at least 16 bytes).
 * @return         xErrno_Ok on success.
 */
XCAPI(xErrno) xHmacMd5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                       uint8_t *digest);

#endif /* XCRYPTO_HMAC_MD5_H */
