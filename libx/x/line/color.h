/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef IC_COLOR_H
#define IC_COLOR_H

//-------------------------------------------------------------
// Terminal colors.
//
// A color is either RGB or an ANSI code. RGB colors have bit 24
// set to distinguish them from the ANSI color palette; xline
// automatically down-converts from RGB on terminals that do not
// support full colors.
//-------------------------------------------------------------

#include "platform.h"

typedef uint32_t ic_color_t;

// Create a color from a 24-bit color value.
ic_private ic_color_t ic_rgb(uint32_t hex);

// Create a color from 8-bit red/green/blue components (clamped to 0..255).
ic_private ic_color_t ic_rgbx(ssize_t r, ssize_t g, ssize_t b);

#define IC_COLOR_NONE (0)
#define IC_RGB(rgb)   (0x1000000 | (uint32_t)(rgb)) // ic_rgb(rgb) -- usable as a constant

// ANSI colors.
// See <https://en.wikipedia.org/wiki/ANSI_escape_code#3-bit_and_4-bit>
#define IC_ANSI_BLACK   (30)
#define IC_ANSI_MAROON  (31)
#define IC_ANSI_GREEN   (32)
#define IC_ANSI_OLIVE   (33)
#define IC_ANSI_NAVY    (34)
#define IC_ANSI_PURPLE  (35)
#define IC_ANSI_TEAL    (36)
#define IC_ANSI_SILVER  (37)
#define IC_ANSI_DEFAULT (39)

#define IC_ANSI_GRAY    (90)
#define IC_ANSI_RED     (91)
#define IC_ANSI_LIME    (92)
#define IC_ANSI_YELLOW  (93)
#define IC_ANSI_BLUE    (94)
#define IC_ANSI_FUCHSIA (95)
#define IC_ANSI_AQUA    (96)
#define IC_ANSI_WHITE   (97)

#define IC_ANSI_DARKGRAY  IC_ANSI_GRAY
#define IC_ANSI_LIGHTGRAY IC_ANSI_SILVER
#define IC_ANSI_MAGENTA   IC_ANSI_FUCHSIA
#define IC_ANSI_CYAN      IC_ANSI_AQUA

#endif // IC_COLOR_H
