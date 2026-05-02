/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
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
