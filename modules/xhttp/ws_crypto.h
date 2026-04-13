/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_crypto.h - WebSocket crypto helpers (SHA-1 + Base64)
 *
 * Provides a unified internal interface for SHA-1 hashing and
 * Base64 encoding, used by the WebSocket handshake to compute
 * the Sec-WebSocket-Accept header value.
 *
 * The actual implementation is selected at compile time based on
 * the available TLS backend (OpenSSL, mbedTLS, or builtin).
 */

#ifndef XHTTP_WS_CRYPTO_H
#define XHTTP_WS_CRYPTO_H

#include <stddef.h>

/** SHA-1 digest size in bytes. */
#define XWS_SHA1_DIGEST_SIZE 20

/**
 * Compute the SHA-1 hash of the input data.
 *
 * @param input   Input data to hash.
 * @param len     Length of input in bytes.
 * @param output  Output buffer (must be >= XWS_SHA1_DIGEST_SIZE).
 */
void xWsSHA1(const unsigned char *input, size_t len,
             unsigned char *output);

/**
 * Base64-encode the input data.
 *
 * @param input    Input data to encode.
 * @param in_len   Length of input in bytes.
 * @param output   Output buffer for the Base64 string.
 * @param out_len  Size of the output buffer in bytes.
 *                 Must be >= ((in_len + 2) / 3) * 4 + 1.
 * @return         Number of characters written (excluding NUL),
 *                 or -1 on error (buffer too small).
 */
int xWsBase64Encode(const unsigned char *input, size_t in_len,
                    char *output, size_t out_len);

#endif /* XHTTP_WS_CRYPTO_H */
