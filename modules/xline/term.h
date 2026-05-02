/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_TERM_H
#define IC_TERM_H

#include "attr.h"
#include "color.h"
#include "mem.h"
#include "platform.h"
#include "tty.h"

// ANSI CSI (Control Sequence Introducer) prefix used by every SGR /
// cursor-movement escape emitted by term.c and term_color.c.
#define IC_CSI "\x1B["

struct term_s;
typedef struct term_s term_t;

// Color support level. Set by `term_new` based on what the terminal
// advertises and the user's IC_COLOR env overrides; consumed by
// term_color.c to pick which SGR escape flavor to emit.
typedef enum palette_e {
  MONOCHROME, // no color
  ANSI8,      // only basic 8 ANSI color     (ESC[<idx>m, idx: 30-37, +10 for
              // background)
  ANSI16, // basic + bright ANSI colors  (ESC[<idx>m, idx: 30-37, 90-97, +10 for
          // background)
  ANSI256, // ANSI 256 color palette      (ESC[38;5;<idx>m)
  ANSIRGB  // direct rgb colors           (ESC[38;2;<r>;<g>;<b>m)
} palette_t;

typedef enum buffer_mode_e {
  UNBUFFERED,
  LINEBUFFERED,
  BUFFERED,
} buffer_mode_t;

// Primitives
ic_private term_t *term_new(alloc_t *mem, tty_t *tty, bool nocolor, bool silent,
                            int fd_out);
ic_private void    term_free(term_t *term);

ic_private bool term_is_interactive(const term_t *term);
ic_private void term_start_raw(term_t *term);
ic_private void term_end_raw(term_t *term, bool force);

ic_private bool term_enable_beep(term_t *term, bool enable);
ic_private bool term_enable_color(term_t *term, bool enable);

ic_private void          term_flush(term_t *term);
ic_private buffer_mode_t term_set_buffer_mode(term_t *term, buffer_mode_t mode);

ic_private void term_write_n(term_t *term, const char *s, ssize_t n);
ic_private void term_write(term_t *term, const char *s);
ic_private void term_writeln(term_t *term, const char *s);
ic_private void term_write_char(term_t *term, char c);

ic_private void term_write_repeat(term_t *term, const char *s, ssize_t count);
ic_private void term_beep(term_t *term);

ic_private bool term_update_dim(term_t *term);

ic_private ssize_t term_get_width(term_t *term);
ic_private ssize_t term_get_height(term_t *term);
ic_private int     term_get_color_bits(term_t *term);

// Helpers
ic_private void term_writef(term_t *term, const char *fmt, ...);
ic_private void term_vwritef(term_t *term, const char *fmt, va_list args);

ic_private void term_left(term_t *term, ssize_t n);
ic_private void term_right(term_t *term, ssize_t n);
ic_private void term_up(term_t *term, ssize_t n);
ic_private void term_down(term_t *term, ssize_t n);
ic_private void term_start_of_line(term_t *term);
ic_private void term_clear_line(term_t *term);
ic_private void term_clear_to_end_of_line(term_t *term);
// ic_private void term_clear_lines_to_end(term_t* term);

ic_private void term_attr_reset(term_t *term);
ic_private void term_underline(term_t *term, bool on);
ic_private void term_reverse(term_t *term, bool on);
ic_private void term_bold(term_t *term, bool on);
ic_private void term_italic(term_t *term, bool on);

ic_private void term_color(term_t *term, ic_color_t color);
ic_private void term_bgcolor(term_t *term, ic_color_t color);

// Exposed so term_color.c can pick the right escape flavor without
// peeking at `struct term_s` directly.
ic_private palette_t term_get_palette(const term_t *term);

// Helpers shared with term.c. term_color.c owns the ANSI 256 palette
// table (some of whose entries term.c patches at init from the
// terminal's actual configuration on Linux/Windows).
ic_private bool            color_is_rgb(ic_color_t color);
ic_private uint32_t        ic_cap8(ssize_t i);
ic_private extern uint32_t ansi256[256];

// Formatted output

ic_private attr_t term_get_attr(const term_t *term);
ic_private void   term_set_attr(term_t *term, attr_t attr);
ic_private void   term_write_formatted(term_t *term, const char *s,
                                       const attr_t *attrs);
ic_private void   term_write_formatted_n(term_t *term, const char *s,
                                         const attr_t *attrs, ssize_t n);

ic_private ic_color_t color_from_ansi256(ssize_t i);

#endif // IC_TERM_H
