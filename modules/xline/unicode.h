/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_UNICODE_H
#define IC_UNICODE_H

//-------------------------------------------------------------
// QUTF-8 (quite-like utf-8) codec.
//
// Internally we always use valid utf-8. If we encounter invalid
// utf-8 bytes (or bytes >= 0x80 from any other encoding) we encode
// these as special code points in the "raw plane" (0xEE000 - 0xEE0FF).
// When decoding we are then able to restore such raw bytes as-is.
// See <https://github.com/koka-lang/koka/blob/master/kklib/include/kklib/string.h>
//-------------------------------------------------------------

#include "platform.h"

typedef uint32_t unicode_t;

ic_private void      unicode_to_qutf8(unicode_t u, uint8_t buf[5]);
ic_private unicode_t unicode_from_qutf8(const uint8_t* s, ssize_t len, ssize_t* nread); // validating

ic_private unicode_t unicode_from_raw(uint8_t c);
ic_private bool      unicode_is_raw(unicode_t u, uint8_t* c);

ic_private bool      utf8_is_cont(uint8_t c);

#endif // IC_UNICODE_H
