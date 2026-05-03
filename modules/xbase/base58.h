/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base58.h - Base58 encoding and decoding (Bitcoin alphabet)
 *
 * Base58 is a binary-to-text encoding that uses an alphabet of 58
 * alphanumeric characters, excluding easily confused characters
 * (0, O, I, l). It is commonly used for compact, human-friendly
 * representation of binary data.
 *
 * Alphabet: 123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz
 */

#ifndef XBASE_BASE58_H
#define XBASE_BASE58_H

#include <xbase/base.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Compute the maximum encoded length for a given input size.
 *
 * Base58 encoding expands data by roughly log(256)/log(58) ≈ 1.37x.
 * This macro returns a safe upper bound (input_len * 138 / 100 + 1),
 * plus 1 byte for the null terminator.
 */
#define XBASE58_ENCODE_MAXLEN(input_len) ((size_t)((input_len) * 138 / 100 + 2))

/**
 * @brief Compute the maximum decoded length for a given encoded size.
 *
 * Base58 decoding shrinks data by roughly log(58)/log(256) ≈ 0.73x.
 * This macro returns a safe upper bound (input_len * 733 / 1000 + 1).
 */
#define XBASE58_DECODE_MAXLEN(input_len) \
  ((size_t)((input_len) * 733 / 1000 + 1))

/**
 * @brief Encode binary data to a Base58 string.
 *
 * @param src      Input binary data.
 * @param src_len  Length of input data in bytes.
 * @param dst      Output buffer for the null-terminated Base58 string.
 * @param dst_len  Pointer to the size of the output buffer. On success,
 *                 updated to the length of the encoded string (excluding
 *                 the null terminator).
 * @return         0 on success, -1 if the output buffer is too small.
 */
XCAPI(int) xBase58Encode(const uint8_t *src, size_t src_len, char *dst,
                         size_t *dst_len);

/**
 * @brief Decode a Base58 string to binary data.
 *
 * @param src      Input null-terminated Base58 string.
 * @param src_len  Length of the input string (excluding null terminator).
 * @param dst      Output buffer for the decoded binary data.
 * @param dst_len  Pointer to the size of the output buffer. On success,
 *                 updated to the length of the decoded data.
 * @return         0 on success, -1 if the output buffer is too small
 *                 or the input contains invalid characters.
 */
XCAPI(int) xBase58Decode(const char *src, size_t src_len, uint8_t *dst,
                         size_t *dst_len);

#endif /* XBASE_BASE58_H */
