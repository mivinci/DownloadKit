/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * wcwidth.h - mk_wcwidth - East-Asian width
 */
#pragma once
#ifndef IC_WCWIDTH_H
#define IC_WCWIDTH_H

// Column width of a Unicode code point, following Markus Kuhn's
// wcwidth() implementation (see wcwidth.c for the full algorithm /
// attribution). Returns 0 for non-spacing/combining characters,
// -1 for C0/C1 control characters, 1 for normal glyphs, 2 for East
// Asian wide / fullwidth glyphs.

#include <stdint.h>

#include "platform.h"

ic_private int mk_wcwidth(int32_t ucs);

#endif // IC_WCWIDTH_H
