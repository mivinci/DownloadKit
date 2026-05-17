/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base64.h - Base64 encoding and decoding (RFC 4648)
 *
 * Supports standard base64 (A-Z, a-z, 0-9, +, /) with '=' padding.
 * Decoding also accepts the URL-safe alphabet (- instead of +,
 * _ instead of /) and input with or without padding.
 *
 * Encode output is NUL-terminated and always includes padding.
 */

#ifndef XBASE_BASE64_H
#define XBASE_BASE64_H

#include <x/base/base.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Compute the maximum encoded length for a given input size.
 *
 * Base64 produces 4 characters per 3 bytes, rounded up, plus
 * '=' padding and a NUL terminator.
 */
#define XBASE64_ENCODE_MAXLEN(input_len) ((size_t)(((input_len) + 2) / 3 * 4 + 1))

/**
 * @brief Compute the maximum decoded length for a given encoded size.
 *
 * Base64 produces at most 3 bytes per 4 characters.
 * This macro returns a safe upper bound for buffer sizing.
 */
#define XBASE64_DECODE_MAXLEN(input_len) ((size_t)((input_len) * 3 / 4 + 2))

/**
 * @brief Encode binary data to a base64 string.
 *
 * Output is NUL-terminated and uses the standard base64 alphabet
 * (A-Z, a-z, 0-9, +, /) with '=' padding.
 *
 * @param src      Input binary data (may be NULL if @p src_len == 0).
 * @param src_len Length of input data in bytes.
 * @param dst      Output buffer for the NUL-terminated base64 string.
 * @param dst_len Pointer to the size of the output buffer. On success,
 *                 updated to the length of the encoded string (excluding
 *                 the NUL terminator).
 * @return         0 on success, -1 if the output buffer is too small.
 */
XCAPI(int) xBase64Encode(const uint8_t *src, size_t src_len, char *dst, size_t *dst_len);

/**
 * @brief Decode a base64 string to binary data.
 *
 * Accepts both the standard alphabet (+, /) and the URL-safe alphabet
 * (-, _). Input with or without '=' padding is accepted.
 *
 * @param src      Input base64 string (NUL-terminated or with known
 *                 length passed in @p src_len).
 * @param src_len Length of the input string in bytes (excluding any
 *                 NUL terminator). May include '=' padding.
 * @param dst      Output buffer for the decoded binary data.
 * @param dst_len Pointer to the size of the output buffer. On success,
 *                 updated to the length of the decoded data.
 * @return         0 on success, -1 if the output buffer is too small
 *                 or the input contains invalid characters or has an
 *                 invalid length.
 */
XCAPI(int) xBase64Decode(const char *src, size_t src_len, uint8_t *dst, size_t *dst_len);

#endif /* XBASE_BASE64_H */
