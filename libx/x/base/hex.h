/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hex.h - Hex encoding and decoding
 *
 * Hex (base16) encoding converts binary data to a printable
 * ASCII string using two hex digits per byte. Both upper- and
 * lower-case hex letters are accepted on decode.
 *
 * Encode output uses lower-case hex digits (0-9, a-f).
 */

#ifndef XBASE_HEX_H
#define XBASE_HEX_H

#include <x/base/base.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Compute the maximum encoded length for a given input size.
 *
 * Hex encoding produces exactly 2 characters per byte, plus a
 * NUL terminator.
 */
#define XHEX_ENCODE_MAXLEN(input_len) ((size_t)((input_len) * 2 + 1))

/**
 * @brief Compute the maximum decoded length for a given encoded size.
 *
 * Hex decoding produces exactly 1 byte per 2 hex characters.
 * Rounds down on odd-length input (caller must ensure even length).
 */
#define XHEX_DECODE_MAXLEN(input_len) ((size_t)((input_len) / 2))

/**
 * @brief Encode binary data to a hex string.
 *
 * Output is NUL-terminated and uses lower-case hex digits (0-9, a-f).
 *
 * @param src      Input binary data (may be NULL if @p src_len == 0).
 * @param src_len Length of input data in bytes.
 * @param dst      Output buffer for the NUL-terminated hex string.
 * @param dst_len Pointer to the size of the output buffer. On success,
 *                 updated to the length of the encoded string (excluding
 *                 the NUL terminator).
 * @return         0 on success, -1 if the output buffer is too small.
 */
XCAPI(int) xHexEncode(const uint8_t *src, size_t src_len, char *dst, size_t *dst_len);

/**
 * @brief Decode a hex string to binary data.
 *
 * Accepts both upper-case and lower-case hex digits. The input
 * must have an even length.
 *
 * @param src      Input NUL-terminated hex string, or a buffer
 *                 with a known length passed in @p src_len.
 * @param src_len Length of the input string in bytes (excluding any
 *                 NUL terminator). Must be even.
 * @param dst      Output buffer for the decoded binary data.
 * @param dst_len Pointer to the size of the output buffer. On success,
 *                 updated to the length of the decoded data.
 * @return         0 on success, -1 if the output buffer is too small
 *                 or the input contains invalid characters or has an
 *                 odd length.
 */
XCAPI(int) xHexDecode(const char *src, size_t src_len, uint8_t *dst, size_t *dst_len);

#endif /* XBASE_HEX_H */
