/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_BBCODE_COLORS_H
#define IC_BBCODE_COLORS_H

// HTML color-name -> ic_color_t lookup table used by the bbcode parser.
// The entries are sorted by name so callers can binary-search.

#include "platform.h"
#include "color.h"

typedef struct style_color_s {
  const char* name;
  ic_color_t  color;
} style_color_t;

#define IC_HTML_COLOR_COUNT (172)

// Sentinel-terminated array of IC_HTML_COLOR_COUNT+1 entries
// (the final entry is {NULL, 0}).
ic_private extern style_color_t html_colors[IC_HTML_COLOR_COUNT + 1];

#endif // IC_BBCODE_COLORS_H
