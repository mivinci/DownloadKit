/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <string.h>

#include "unicode.h"

//-------------------------------------------------------------
// QUTF-8 codec.
// Raw bytes are code points 0xEE000 - 0xEE0FF.
// See
// <https://github.com/koka-lang/koka/blob/master/kklib/include/kklib/string.h>
//-------------------------------------------------------------

#define IC_UNICODE_RAW ((unicode_t)(0xEE000U))

ic_private unicode_t unicode_from_raw(uint8_t c) {
  return (IC_UNICODE_RAW + c);
}

ic_private bool unicode_is_raw(unicode_t u, uint8_t *c) {
  if (u >= IC_UNICODE_RAW && u <= IC_UNICODE_RAW + 0xFF) {
    *c = (uint8_t)(u - IC_UNICODE_RAW);
    return true;
  } else {
    return false;
  }
}

ic_private void unicode_to_qutf8(unicode_t u, uint8_t buf[5]) {
  memset(buf, 0, 5);
  if (u <= 0x7F) {
    buf[0] = (uint8_t)u;
  } else if (u <= 0x07FF) {
    buf[0] = (0xC0 | ((uint8_t)(u >> 6)));
    buf[1] = (0x80 | (((uint8_t)u) & 0x3F));
  } else if (u <= 0xFFFF) {
    buf[0] = (0xE0 | ((uint8_t)(u >> 12)));
    buf[1] = (0x80 | (((uint8_t)(u >> 6)) & 0x3F));
    buf[2] = (0x80 | (((uint8_t)u) & 0x3F));
  } else if (u <= 0x10FFFF) {
    if (unicode_is_raw(u, &buf[0])) {
      buf[1] = 0;
    } else {
      buf[0] = (0xF0 | ((uint8_t)(u >> 18)));
      buf[1] = (0x80 | (((uint8_t)(u >> 12)) & 0x3F));
      buf[2] = (0x80 | (((uint8_t)(u >> 6)) & 0x3F));
      buf[3] = (0x80 | (((uint8_t)u) & 0x3F));
    }
  }
}

// is this a utf8 continuation byte?
ic_private bool utf8_is_cont(uint8_t c) {
  return ((c & 0xC0) == 0x80);
}

ic_private unicode_t unicode_from_qutf8(const uint8_t *s, ssize_t len,
                                        ssize_t *count) {
  unicode_t c0 = 0;
  if (len <= 0 || s == NULL) {
    goto fail;
  }
  // 1 byte
  c0 = s[0];
  if (c0 <= 0x7F && len >= 1) {
    if (count != NULL) *count = 1;
    return c0;
  } else if (c0 <= 0xC1) { // invalid continuation byte or invalid 0xC0, 0xC1
    goto fail;
  }
  // 2 bytes
  else if (c0 <= 0xDF && len >= 2 && utf8_is_cont(s[1])) {
    if (count != NULL) *count = 2;
    return (((c0 & 0x1F) << 6) | (s[1] & 0x3F));
  }
  // 3 byte encoding; reject overlong and utf-16 surrogate halves (0xD800 -
  // 0xDFFF)
  else if (len >= 3 && ((c0 == 0xE0 && s[1] >= 0xA0 && s[1] <= 0xBF &&
                         utf8_is_cont(s[2])) ||
                        (c0 >= 0xE1 && c0 <= 0xEF && c0 != 0xED &&
                         utf8_is_cont(s[1]) && utf8_is_cont(s[2])) ||
                        (c0 == 0xED && s[1] > 0x80 && s[1] <= 0x9F &&
                         utf8_is_cont(s[2])))) {
    if (count != NULL) *count = 3;
    return (((c0 & 0x0F) << 12) | ((unicode_t)(s[1] & 0x3F) << 6) |
            (s[2] & 0x3F));
  }
  // 4 bytes: reject overlong
  else if (len >= 4 && (((c0 == 0xF0 && s[1] >= 0x90 && s[1] <= 0xBF &&
                          utf8_is_cont(s[2]) && utf8_is_cont(s[3])) ||
                         (c0 >= 0xF1 && c0 <= 0xF3 && utf8_is_cont(s[1]) &&
                          utf8_is_cont(s[2]) && utf8_is_cont(s[3])) ||
                         (c0 == 0xF4 && s[1] >= 0x80 && s[1] <= 0x8F &&
                          utf8_is_cont(s[2]) && utf8_is_cont(s[3]))))) {
    if (count != NULL) *count = 4;
    return (((c0 & 0x07) << 18) | ((unicode_t)(s[1] & 0x3F) << 12) |
            ((unicode_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F));
  }
fail:
  if (count != NULL) *count = 1;
  return unicode_from_raw(s[0]);
}
