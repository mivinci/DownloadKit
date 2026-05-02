/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * unicode.h - QUTF-8 codec and Unicode helpers
 */
#pragma once
#ifndef IC_UNICODE_H
#define IC_UNICODE_H

/* ── QUTF-8 (quite-like utf-8) codec. Internally we always use valid utf-8. If
 * we encounter invalid utf-8 bytes (or bytes >= 0x80 from any other encoding)
 * we encode these as special code points in the "raw plane" (0xEE000 -
 * 0xEE0FF). When decoding we are then able to restore such raw bytes as-is. See
 * <https://github.com/koka-lang/koka/blob/master/kklib/include/kklib/string.h>
 * ── */

#include "platform.h"

typedef uint32_t unicode_t;

ic_private void      unicode_to_qutf8(unicode_t u, uint8_t buf[5]);
ic_private unicode_t unicode_from_qutf8(const uint8_t *s, ssize_t len,
                                        ssize_t *nread); // validating

ic_private unicode_t unicode_from_raw(uint8_t c);
ic_private bool      unicode_is_raw(unicode_t u, uint8_t *c);

ic_private bool utf8_is_cont(uint8_t c);

#endif // IC_UNICODE_H
