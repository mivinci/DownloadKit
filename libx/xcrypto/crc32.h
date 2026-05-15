/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * crc32.h - CRC-32 interface (ISO 3309 / ITU-T V.42)
 *
 * Provides a one-shot CRC-32 function using the standard polynomial
 * 0xEDB88320 (reflected). Pure-C implementation, no external
 * dependencies required.
 */

#ifndef XCRYPTO_CRC32_H
#define XCRYPTO_CRC32_H

#include <xbase/base.h>

#include <stddef.h>
#include <stdint.h>

/* ───────────────────── One-shot API ───────────────────── */

/**
 * @brief Compute CRC-32 of a buffer in one call.
 *
 * Uses the standard polynomial 0xEDB88320 (reflected).
 *
 * @param data  Input data.
 * @param len   Length of data in bytes.
 * @return      CRC-32 checksum.
 */
XCAPI(uint32_t) xCrc32(const uint8_t *data, size_t len);

#endif /* XCRYPTO_CRC32_H */
