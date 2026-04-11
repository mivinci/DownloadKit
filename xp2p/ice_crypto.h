/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_crypto.h - Crypto helpers for xIce (HMAC-SHA1 + CRC-32)
 *
 * Provides HMAC-SHA1 for STUN MESSAGE-INTEGRITY and CRC-32 for
 * STUN FINGERPRINT. The builtin implementation requires no external
 * dependencies.
 */

#ifndef XP2P_ICE_CRYPTO_H
#define XP2P_ICE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/** MD5 digest size in bytes. */
#define XICE_MD5_DIGEST_SIZE 16

/** SHA-1 block size in bytes. */
#define XICE_SHA1_BLOCK_SIZE 64

/** SHA-1 digest size in bytes. */
#define XICE_SHA1_DIGEST_SIZE 20

/**
 * @brief Compute MD5 hash (RFC 1321).
 *
 * Used for TURN long-term credential key derivation.
 *
 * @param input   Input data.
 * @param len     Length of input in bytes.
 * @param output  Output buffer (must be >= XICE_MD5_DIGEST_SIZE).
 */
void xIceMD5(const uint8_t *input, size_t len, uint8_t *output);

/**
 * @brief Compute SHA-1 hash.
 *
 * @param input   Input data.
 * @param len     Length of input in bytes.
 * @param output  Output buffer (must be >= XICE_SHA1_DIGEST_SIZE).
 */
void xIceSHA1(const uint8_t *input, size_t len, uint8_t *output);

/**
 * @brief Compute HMAC-SHA1 (RFC 2104).
 *
 * Used for STUN MESSAGE-INTEGRITY attribute.
 *
 * @param key      HMAC key.
 * @param key_len  Length of key in bytes.
 * @param data     Input data.
 * @param data_len Length of data in bytes.
 * @param output   Output buffer (must be >= XICE_SHA1_DIGEST_SIZE).
 */
void xIceHmacSHA1(const uint8_t *key, size_t key_len, const uint8_t *data,
                  size_t data_len, uint8_t *output);

/**
 * @brief Compute CRC-32 (ISO 3309 / ITU-T V.42).
 *
 * Used for STUN FINGERPRINT attribute.
 *
 * @param data  Input data.
 * @param len   Length of data in bytes.
 * @return      CRC-32 checksum.
 */
uint32_t xIceCrc32(const uint8_t *data, size_t len);

#endif /* XP2P_ICE_CRYPTO_H */
