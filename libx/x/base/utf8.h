/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * utf8.h - UTF-8 validation
 */

#ifndef XBASE_UTF8_H
#define XBASE_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <x/base/base.h>

/**
 * @brief Validate that a byte buffer is valid UTF-8.
 *
 * Checks all structural rules:
 *   - Correct multi-byte sequence lengths (1–4 bytes)
 *   - Continuation bytes (10xxxxxx) only in trailing positions
 *   - No overlong encodings
 *   - No surrogate code points (U+D800..U+DFFF)
 *   - No code points above U+10FFFF
 *
 * @param data  Pointer to bytes (NULL with len=0 is valid → returns true).
 * @param len   Number of bytes to check.
 * @return true if the buffer is valid UTF-8, false otherwise.
 */
XCAPI(bool) xValidateUtf8(const char *data, size_t len);

#endif /* XBASE_UTF8_H */
