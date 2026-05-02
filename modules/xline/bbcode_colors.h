/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bbcode_colors.h - BBCode color/style definitions
 */
#pragma once
#ifndef IC_BBCODE_COLORS_H
#define IC_BBCODE_COLORS_H

// HTML color-name -> ic_color_t lookup table used by the bbcode parser.
// The entries are sorted by name so callers can binary-search.

#include "color.h"
#include "platform.h"

typedef struct style_color_s {
  const char *name;
  ic_color_t  color;
} style_color_t;

#define IC_HTML_COLOR_COUNT (172)

// Sentinel-terminated array of IC_HTML_COLOR_COUNT+1 entries
// (the final entry is {NULL, 0}).
ic_private extern style_color_t html_colors[IC_HTML_COLOR_COUNT + 1];

#endif // IC_BBCODE_COLORS_H
