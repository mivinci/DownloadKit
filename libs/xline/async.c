/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen (original isocline)
  Copyright (c) 2026, moo contributors (xline async additions)
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE.isocline" file at the root of this module.
-----------------------------------------------------------------------------*/
//-------------------------------------------------------------
// FD-level async line editing.
//
// Relies on the synchronous edit primitives declared in edit.h
// (editor_t, edit_init, edit_dispatch_key, edit_finalize, edit_refresh,
// edit_write_prompt) and tty.c (tty_read_timeout, tty_start_raw,
// tty_end_raw, tty_fd).
//-------------------------------------------------------------

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/ioctl.h>
#endif

#include <xbase/log.h>
#include "edit.h"
#include "env.h"
#include "line.h"
#include "platform.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"

//-------------------------------------------------------------
// Trace logging to a side file. Two-stage gate:
//   1. Compile-time: XLINE_TRACE_ENABLED (see edit.h). When 0 the
//      whole block below is stripped and every xline_trace(...) call
//      site expands to (void)0 via the header's macro fallback.
//   2. Runtime: the XLINE_TRACE env var holds the log path; if unset
//      the file is never opened and every call returns immediately.
// Kept behind a flag rather than deleted because bringing it back is
// the fastest way to diagnose the next above-region / cursor-geometry
// regression. DO NOT remove.
//-------------------------------------------------------------

#if XLINE_TRACE_ENABLED

static FILE *g_xline_trace_fp      = NULL;
static int   g_xline_trace_tried   = 0;

static FILE *xline_trace_file(void) {
  if (g_xline_trace_tried) return g_xline_trace_fp;
  g_xline_trace_tried = 1;
  const char *path = getenv("XLINE_TRACE");
  if (path == NULL || *path == '\0') return NULL;
  g_xline_trace_fp = fopen(path, "w");
  if (g_xline_trace_fp != NULL) {
    setvbuf(g_xline_trace_fp, NULL, _IOLBF, 0);
    fprintf(g_xline_trace_fp, "# xline trace opened\n");
  }
  return g_xline_trace_fp;
}

ic_private void xline_trace(const char *fmt, ...) {
  FILE *fp = xline_trace_file();
  if (fp == NULL) return;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  fprintf(fp, "[%ld.%06ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fputc('\n', fp);
}

// Dump raw bytes with control-char escaping. Useful for recording exactly
// what gets written to / read from the tty around a suspected race.
ic_private void xline_trace_bytes(const char *label, const char *data,
                                  ssize_t len) {
  FILE *fp = xline_trace_file();
  if (fp == NULL) return;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  fprintf(fp, "[%ld.%06ld] %s: len=%zd bytes=[", (long)ts.tv_sec,
          ts.tv_nsec / 1000, label ? label : "bytes", len);
  for (ssize_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];
    if (c == '\\')       fputs("\\\\", fp);
    else if (c == '\n')  fputs("\\n", fp);
    else if (c == '\r')  fputs("\\r", fp);
    else if (c == '\t')  fputs("\\t", fp);
    else if (c == 0x1b)  fputs("\\e", fp);
    else if (c < 0x20 || c == 0x7f) fprintf(fp, "\\x%02x", c);
    else                 fputc((int)c, fp);
  }
  fputs("]\n", fp);
}

#endif // XLINE_TRACE_ENABLED

//-------------------------------------------------------------
// Handle state
//-------------------------------------------------------------

typedef enum xline_async_state_e {
  XLINE_ASYNC_INIT = 0,      // freshly begun; Begin/Step not yet taken/ended
  XLINE_ASYNC_RUNNING,       // still feeding keys
  XLINE_ASYNC_DONE_LINE,     // user pressed Enter (or cancel returning empty
                             // string)
  XLINE_ASYNC_DONE_EOF,      // Ctrl-D on empty input or STOP
  XLINE_ASYNC_DONE_INTERRUPT, // user pressed Ctrl-C / ^G
  XLINE_ASYNC_DONE_ERROR,    // unrecoverable error
} xline_async_state_t;

XDEF_STRUCT(xLineHandle_) {
  ic_env_t           *env;
  editor_t            eb;
  xline_async_state_t state;
  code_t              last_code;    // last code that drove us into a terminal state
  char               *taken_line;   // cached result after finalize
  // Token-level streaming above the prompt.
  //
  // The terminal screen is logically split into an "above region" (everything
  // written by xLinePrintAbove[Chunk]) and the "edit region" (prompt + input).
  // Invariant: edit_refresh() paints prompt+input starting at column 0 of a
  // fresh line, so whatever sits on that line before edit_refresh runs would
  // be clobbered.
  //
  // To support true token-by-token streaming (append to the current trailing
  // line, wrap at term width, flush newlines in-place) while keeping that
  // invariant, we bookkeep the last still-open line of the above region:
  //
  //   last_line      — bytes of the above region's trailing line that have
  //                    been written to the screen but not yet terminated by
  //                    a '\n' (i.e. the part the next chunk may extend).
  //   last_line_rows — how many screen rows those bytes occupy once wrapped
  //                    at the current terminal width (>= 1 when last_line
  //                    is non-empty, 0 otherwise).
  //
  // On each chunk, we:
  //   1. wipe the edit region;
  //   2. undo the '\n' separator we wrote last time and erase last_line's
  //      screen rows;
  //   3. append the new bytes, emit everything up to the final '\n' (if any)
  //      and keep the remainder as the new last_line;
  //   4. rewrite last_line and a separator '\n';
  //   5. repaint prompt+input via edit_refresh().
  //
  // Visual effect: tokens appear to flow onto the same line, wrapping
  // naturally; completed lines scroll up. The prompt never jitters onto a
  // "taken" line.
  stringbuf_t        *last_line;
  ssize_t             last_line_rows;
  ssize_t             last_line_col;  // cached column at tail of last_line

  // Active SGR stack for the above region.
  //
  // `last_line` alone is not enough to reconstruct the styling that should
  // apply to a newly-appended chunk: if the caller wrote '\x1b[2m' early,
  // then several full lines of text followed by a '\n' (which rotates the
  // trailing line out of `last_line`), the faint attribute is still active
  // as far as the terminal is concerned, but a subsequent term_attr_reset
  // (done on every sneak fast path) would clear it with no way to restore.
  //
  // `active_sgr` accumulates every SGR sequence ever emitted to the above
  // region since the last \x1b[0m / \x1b[m reset. Whenever we need to
  // re-establish styling after term_attr_reset (sneak path), or prepend it
  // to a slow-path rewrite so the re-emitted last_line inherits it, we
  // replay this buffer. See xline_track_sgr() for the bookkeeping.
  stringbuf_t        *active_sgr;

  // Below panel (persistent UI area underneath the edit region).
  //
  // Content that survives across above-region streaming. We piggy-back on
  // the editor's `extra` buffer — it is already wired into edit_refresh()
  // to paint below the input, grow `cur_rows`, and be overwritten on
  // shrink. The trick is that `eb->extra` is a shared slot (completion
  // menus and history search also use it); we re-inject our panel every
  // time we drive a refresh, so whenever those other features finish and
  // clear `eb->extra`, the below panel re-appears on the next paint.
  //
  // `below_title` may be NULL. An empty `below_body` means the panel is
  // hidden.
  stringbuf_t        *below_title;
  stringbuf_t        *below_body;

  // Last observed terminal geometry. Used to distinguish a real SIGWINCH
  // (columns/rows actually changed) from spurious ones (the terminal
  // emulator re-raises SIGWINCH on focus/theme events, and
  // tty_term_resize_event also hard-returns true on platforms without a
  // native resize event). Only real changes trigger edit_resize bookkeeping.
  ssize_t             last_term_w;
  ssize_t             last_term_h;

  // Bracketed paste state.
  //
  // We enable DECSET ?2004 in xLineBegin, so xterm-compatible terminals
  // wrap pasted bytes in CSI 200 ~ ... CSI 201 ~. tty_esc.c decodes
  // those markers into KEY_EVENT_PASTE_BEGIN / KEY_EVENT_PASTE_END; the
  // drain loop in xLineStep flips in_paste and, while true, routes
  // incoming codes through the "insert literal" path instead of
  // edit_dispatch_key. This is required because edit_dispatch_key
  // treats KEY_ENTER (== '\r') as "submit the line", and most terminal
  // emulators on macOS / Linux send '\r' for newlines inside a paste —
  // without this branch a multi-line paste would be truncated at the
  // first newline and the remainder would become the prefix of the
  // next submission.
  //
  // While in_paste is true we also suspend the per-keystroke refresh
  // (edit_refresh / edit_refresh_hint short-circuit when
  // eb->suspend_refresh is set) and issue exactly one refresh after
  // the terminator. For a 1 KB paste that turns O(N) full repaints
  // into a single one, eliminating the otherwise very visible flicker.
  bool                in_paste;
};

// Max visible lines for the below panel. Longer bodies are truncated with a
// "... (N more lines)" marker.
#define XLINE_BELOW_PANEL_MAX_ROWS 10

// Only one live session at a time (requirement 4.4).
static xLineHandle_ *g_live_session = NULL;

//-------------------------------------------------------------
// Below panel: re-inject into eb->extra before each refresh.
//-------------------------------------------------------------

// Count '\n' occurrences in [s, s+len).
static ssize_t xline_count_newlines(const char *s, ssize_t len) {
  ssize_t n = 0;
  for (ssize_t i = 0; i < len; i++) {
    if (s[i] == '\n') n++;
  }
  return n;
}

// Install the current below panel into eb->extra as bbcode. Takes ownership
// of the slot (clobbers any previous content). When the panel is empty the
// slot is just cleared, which matches the "no below region" rendering.
static void xline_sync_extra_from_below(xLineHandle_ *h) {
  editor_t   *eb      = &h->eb;
  const char *body    = (h->below_body != NULL ? sbuf_string(h->below_body) : NULL);
  ssize_t     body_n  = (h->below_body != NULL ? sbuf_len(h->below_body) : 0);

  sbuf_clear(eb->extra);
  if (body == NULL || body_n == 0) return;

  // Header row:
  //   - With title: "── <title> ──────────…" stretched to full width, so the
  //     rule lands in the same column as the untitled case. The leading
  //     "── <title> " segment is fixed; the trailing run expands to fill
  //     whatever's left of the terminal width.
  //   - Without title: just the rule, same full width.
  //
  // We cap the total width at termw-2 (not -1 like edit_completion.c):
  // `-1` was still overflowing on some emulators (iTerm2 showed a
  // trailing ↵ and the row wrapped), presumably because the bbcode
  // renderer can emit a CR or reset sequence that nudges the final
  // column. Width is sampled from h->env->term (set in xLineOpen) and
  // falls back to a safe default if env isn't wired up yet.
  //
  // Both branches use a raw ANSI faint escape (\x1b[2m … \x1b[22m)
  // rather than the bbcode [ic-diminish] tag: despite its name,
  // ic-diminish is defined as ansi-lightgray in line.c — a concrete
  // foreground colour that renders nearly white on dark terminals. We
  // want the actual "dim" SGR attribute used by the startup banner
  // border, which is \x1b[2m. bbcode has no built-in faint/dim style,
  // so we emit the SGR bytes directly; the renderer passes unknown
  // escape sequences through. Keeping both branches on the same
  // escape keeps the title rule and untitled rule visually identical.
  //
  // Titles are emitted as raw bytes (no [!pre] wrapper), so callers
  // must pass plain text — no bbcode tags, no control characters.
  // All current callers pass ASCII literals (e.g. "bypass") so this
  // is fine in practice.
  ssize_t rule_w = (h->env != NULL ? term_get_width(h->env->term) - 2 : 78);
  if (rule_w < 4)  rule_w = 4;
  if (rule_w > 512) rule_w = 512; // defensive cap; no real terminal is wider

  sbuf_append(eb->extra, "\x1b[2m");
  if (h->below_title != NULL && sbuf_len(h->below_title) > 0) {
    const char *title   = sbuf_string(h->below_title);
    // "── " (2 glyphs + 1 space) + title + " "  → fixed prefix width.
    ssize_t     title_w = str_column_width(title);
    ssize_t     prefix  = 2 + 1 + title_w + 1;
    ssize_t     tail    = rule_w - prefix;
    if (tail < 3) tail = 3; // always leave a minimum visible rule stub
    sbuf_appendf(eb->extra, "\xe2\x94\x80\xe2\x94\x80 %s ", title);
    for (ssize_t i = 0; i < tail; i++) {
      sbuf_append(eb->extra, "\xe2\x94\x80");
    }
  } else {
    for (ssize_t i = 0; i < rule_w; i++) {
      sbuf_append(eb->extra, "\xe2\x94\x80");
    }
  }
  sbuf_append(eb->extra, "\x1b[22m\n");
  // Breathing room under a titled header; untitled rules stay flush
  // because the rule itself is already visually part of the body.
  if (h->below_title != NULL && sbuf_len(h->below_title) > 0) {
    sbuf_append(eb->extra, "\n");
  }

  // Clip to XLINE_BELOW_PANEL_MAX_ROWS logical lines. Count newlines; if the
  // body has fewer than max newlines the body is shown verbatim. Else split
  // after the Nth newline and append a truncation marker.
  ssize_t total_lines = xline_count_newlines(body, body_n);
  if (body_n > 0 && body[body_n - 1] != '\n') total_lines++;

  if (total_lines <= XLINE_BELOW_PANEL_MAX_ROWS) {
    sbuf_append(eb->extra, "[!pre]");
    sbuf_append_n(eb->extra, body, body_n);
    sbuf_append(eb->extra, "[/pre]");
  } else {
    // Find end offset of the Nth line.
    ssize_t cut = 0;
    ssize_t seen = 0;
    for (ssize_t i = 0; i < body_n && seen < XLINE_BELOW_PANEL_MAX_ROWS; i++) {
      if (body[i] == '\n') {
        seen++;
        cut = i + 1;
      }
    }
    sbuf_append(eb->extra, "[!pre]");
    sbuf_append_n(eb->extra, body, cut);
    sbuf_append(eb->extra, "[/pre]");
    ssize_t rest = total_lines - XLINE_BELOW_PANEL_MAX_ROWS;
    sbuf_appendf(eb->extra, "[ic-diminish]... (%zd more line%s)[/]",
                 rest, rest == 1 ? "" : "s");
  }
}

// Exposed (internally) so the synchronous readline path can assert non-reentry.
ic_private bool xline_async_is_live(void) {
  return g_live_session != NULL;
}

// Pre-refresh hook wired into ic_env_t.refresh_prepare. Invoked at the top
// of every edit_refresh so we can re-inject the below panel into
// eb->extra — completion menus and history search also use that slot for
// their own overlays, and without this we'd lose the panel as soon as one
// of those subsystems runs a refresh.
//
// We skip when another subsystem is actively using eb->extra for the
// current frame:
//   - completion menu is live (completions_count > 0): its own refresh
//     loop clears eb->extra and paints the menu; our panel stays out of
//     the way until the menu exits and clears completions.
//   - a hint-help line is pending (eb->hint_help non-empty): the inline
//     hint info wins the extra slot for this one frame.
// In every other case we reclaim the slot and (re)paint the panel, which
// naturally recovers after completion/hint frames finish.
static void xline_refresh_prepare(void *arg, void *eb_opaque) {
  xLineHandle_ *h  = (xLineHandle_ *)arg;
  editor_t     *eb = (editor_t *)eb_opaque;
  if (h == NULL || eb == NULL) return;
  if (eb != &h->eb) return; // safety: only act on our own editor

  // Skip when the hint-help overlay currently owns eb->extra. The
  // completion menu is handled separately: it suspends this hook around
  // its own edit_refresh() call so its menu contents survive.
  if (sbuf_len(eb->hint_help) > 0) {
    return;
  }
  xline_sync_extra_from_below(h);
}

//-------------------------------------------------------------
// Token-level streaming helpers.
//
// The edit region (prompt + input) is repainted on top of the above region
// after every flush. Wiping it is safe because we always write a '\n'
// separator between the last above-region bytes and the prompt before
// calling edit_refresh — and we remember to undo that '\n' (plus any rows
// the current last_line already occupies) on the next flush.
//-------------------------------------------------------------

static void xline_wipe_edit_region(ic_env_t *env, editor_t *eb) {
  term_attr_reset(env->term);
  term_up(env->term, eb->cur_row);   // move up to the prompt's first row
  term_write(env->term, "\r\x1b[J"); // clear from here to end of screen
  eb->cur_row  = 0;
  eb->cur_rows = 1;
}

// Count how many screen rows `s` (len bytes, no trailing '\n' expected)
// would occupy when painted starting at column 0 under the current term
// width. Returns 0 when `s` is empty.
//
// We deliberately do NOT delegate to sbuf_get_rc_at_pos / str_for_each_row
// here: those helpers serve the line editor, which reserves one cell for
// the cursor (they wrap when `col + w + 1 >= termw`). For pure above-region
// output no cursor lives inside the text, so reusing the editor's counter
// *overestimates* by one row every time the tail character lands in the
// last column. An overestimated last_line_rows causes xline_erase_trailing
// to term_up() one row too many on the next chunk, and the follow-up
// \x1b[J then wipes the bottom of already-committed scrollback — which
// is exactly the "thinking ate N rows" symptom we chased.
static ssize_t xline_count_rows(ic_env_t *env, const char *s, ssize_t len) {
  if (len <= 0) return 0;
  ssize_t termw = term_get_width(env->term);
  if (termw <= 0) termw = 80;
  ssize_t col  = 0;
  ssize_t rows = 1;
  ssize_t i    = 0;
  while (i < len) {
    ssize_t w    = 0;
    ssize_t next = str_next_ofs(s, len, i, &w);
    if (next <= 0) break;
    if (s[i] == '\n') {
      rows++;
      col = 0;
      i += next;
      continue;
    }
    // Mirror the terminal's autowrap rule: a visible glyph of width `w`
    // fits iff col + w <= termw. When it doesn't fit we wrap BEFORE
    // drawing it, then place it at column w on the new row.
    if (w > 0 && col + w > termw) {
      rows++;
      col = w;
    } else {
      col += w;
    }
    i += next;
  }
  return rows;
}

// Same traversal as xline_count_rows, but also returns the column (0-based)
// at which the *next* character would land on the final row. This is what
// the sneak fast path in xLinePrintAboveChunk needs to position the cursor
// at the tail of last_line before appending a chunk without redrawing the
// edit region below. Returns 0 rows and *out_col=0 for empty input.
static ssize_t xline_count_rows_and_last_col(ic_env_t *env, const char *s,
                                             ssize_t len, ssize_t *out_col) {
  if (out_col != NULL) *out_col = 0;
  if (len <= 0) return 0;
  ssize_t termw = term_get_width(env->term);
  if (termw <= 0) termw = 80;
  ssize_t col  = 0;
  ssize_t rows = 1;
  ssize_t i    = 0;
  while (i < len) {
    // Skip ANSI escape sequences — they emit no cells, but
    // str_next_ofs + char_width would count them as literal ASCII
    // (ESC, '[', digits, 'm' …) and inflate the width by 3-5 columns
    // per SGR. In a streaming markdown render last_line accumulates
    // many SGR pairs, so the error compounds: we decide "this chunk
    // wraps a new row" when the terminal actually still has room, and
    // the sneak fast path then pessimistically bails out (pre-fix) or
    // now draws at a slightly off-by-N column. Either way the cost is
    // re-scans over an oversized last_line every tick. Dropping escape
    // bytes out of the count restores truth.
    if (s[i] == '\x1B' && i + 1 < len) {
      char next = s[i + 1];
      if (next == '[') {
        // CSI: ESC [ <params> <final 0x40-0x7E>
        ssize_t j = i + 2;
        while (j < len) {
          unsigned char c = (unsigned char)s[j];
          j++;
          if (c >= 0x40 && c <= 0x7E) break; // final byte
        }
        i = j;
        continue;
      }
      if (next == ']') {
        // OSC: ESC ] ... (BEL | ESC '\')
        ssize_t j = i + 2;
        while (j < len) {
          unsigned char c = (unsigned char)s[j];
          if (c == 0x07) { j++; break; }
          if (c == 0x1B && j + 1 < len && s[j + 1] == '\\') { j += 2; break; }
          j++;
        }
        i = j;
        continue;
      }
      // Two-byte ESC sequence (DECSC \e7, DECRC \e8, etc.) — skip both.
      i += 2;
      continue;
    }
    ssize_t w    = 0;
    ssize_t next = str_next_ofs(s, len, i, &w);
    if (next <= 0) break;
    if (s[i] == '\n') {
      rows++;
      col = 0;
      i += next;
      continue;
    }
    if (w > 0 && col + w > termw) {
      rows++;
      col = w;
    } else {
      col += w;
    }
    i += next;
  }
  if (out_col != NULL) *out_col = col;
  return rows;
}

// Variant of xline_count_rows_and_last_col that starts from an arbitrary
// initial column instead of 0.  Used by the sneak fast path in
// xLinePrintAboveChunk to compute row/column deltas from the *chunk alone*
// (O(chunk_len)) using the cached last_line_col as the starting column,
// avoiding the O(last_line_len) re-scan of the entire trailing line.
//
// Returns the number of *additional* screen rows consumed (i.e. 0 when the
// chunk fits on the same row as the initial column), and writes the final
// column to *out_col.
static ssize_t xline_count_rows_from_col(ic_env_t *env, const char *s,
                                          ssize_t len, ssize_t termw,
                                          ssize_t init_col,
                                          ssize_t *out_col) {
  if (out_col != NULL) *out_col = init_col;
  if (len <= 0) return 0;
  if (termw <= 0) termw = 80;
  ssize_t col  = init_col;
  ssize_t rows = 0;  // additional rows beyond the one init_col sits on
  ssize_t i    = 0;
  while (i < len) {
    if (s[i] == '\x1B' && i + 1 < len) {
      char next = s[i + 1];
      if (next == '[') {
        ssize_t j = i + 2;
        while (j < len) {
          unsigned char c = (unsigned char)s[j];
          j++;
          if (c >= 0x40 && c <= 0x7E) break;
        }
        i = j;
        continue;
      }
      if (next == ']') {
        ssize_t j = i + 2;
        while (j < len) {
          unsigned char c = (unsigned char)s[j];
          if (c == 0x07) { j++; break; }
          if (c == 0x1B && j + 1 < len && s[j + 1] == '\\') { j += 2; break; }
          j++;
        }
        i = j;
        continue;
      }
      i += 2;
      continue;
    }
    ssize_t w    = 0;
    ssize_t next = str_next_ofs(s, len, i, &w);
    if (next <= 0) break;
    if (s[i] == '\n') {
      rows++;
      col = 0;
      i += next;
      continue;
    }
    if (w > 0 && col + w > termw) {
      rows++;
      col = w;
    } else {
      col += w;
    }
    i += next;
  }
  if (out_col != NULL) *out_col = col;
  return rows;
}

// Rewind the cursor to the start of the above region's trailing line and
// clear from there to the end of the screen. After this call the terminal
// write head is positioned exactly where the next above-region byte should
// land. `h->last_line_rows` is the number of screen rows occupied by the
// old trailing line; the separator '\n' that follows it is one extra row.
//
// Safety clamp: if the terminal auto-scrolled after we wrote last_line
// (its upper rows moved into scrollback), an unguarded
// term_up(last_line_rows) would be clipped by the terminal to row 1 and
// the follow-up \x1b[J would wipe every visible row above the cursor
// (previous reply, user echo, etc.). To prevent that we issue one
// on-demand CPR round-trip *only when* we're about to climb more than
// one row, and cap the climb at "current row - 1". CPR cost (5-20 ms)
// is acceptable here because this branch is only taken when last_line
// wraps to 2+ screen rows, which is rare.
static void xline_erase_trailing(xLineHandle_ *h) {
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  xline_trace("erase_trailing: cur_row=%zd cur_rows=%zd last_line_rows=%zd",
              (ssize_t)eb->cur_row, (ssize_t)eb->cur_rows, h->last_line_rows);
  xline_wipe_edit_region(env, eb);
  if (h->last_line_rows <= 0) return;

  // Step up onto the separator row (the '\n' we emitted after last_line)
  // and above, then clear to end-of-screen.
  //
  // We deliberately do NOT issue a CPR (\x1B[6n) round-trip here. The
  // old code did, "to clamp term_up so we cannot overshoot into
  // already-committed content above" when the terminal had auto-
  // scrolled. In practice two things made that harmful:
  //
  //   1. CPR is a synchronous tty read. On every slow-path tick we
  //      paid one round-trip (single-digit to tens of ms on local
  //      terminals, way more over ssh). Under a bursty streaming
  //      workload that snowballs into visible "the output froze and
  //      keys don't register" — the main loop spends all its time
  //      blocked on the CPR and never gets back to input dispatch.
  //
  //   2. The clamp it enabled was the wrong shape. term_up is
  //      already bounded by the terminal itself — the cursor can't
  //      climb above row 1 regardless of what we ask for. The worst
  //      that happens without the clamp is that \x1B[J clears a few
  //      rows we're about to rewrite anyway; no user-visible content
  //      is lost because xline_emit_bytes re-emits the full trailing
  //      line immediately after.
  //
  // So: cap `up` at (term_height - 1) defensively (term_up(0) is a
  // no-op, this just avoids asking for a silly number when the
  // bookkeeping is briefly stale after a resize) and move on.
  ssize_t up = h->last_line_rows;
  ssize_t h_rows = term_get_height(env->term);
  if (h_rows > 1 && up > h_rows - 1) up = h_rows - 1;
  if (up > 0) {
    term_up(env->term, up);
  }
  term_write(env->term, "\r\x1b[J");
}

// Walk `data` and update `dst` to reflect the cumulative SGR state after
// emitting those bytes. Any complete SGR sequence (ESC '[' ... 'm') is
// appended verbatim; a reset (ESC '[' 'm' or ESC '[' '0' 'm', possibly with
// leading zeros like '00') clears `dst` instead of being appended. All
// non-SGR bytes are ignored. Malformed / truncated sequences are dropped.
//
// The result is a minimal "replay prefix" that, when written to a terminal
// after a term_attr_reset, restores the styling that was in effect at the
// end of `data` — modulo compound SGRs where a later sequence only
// partially cancels an earlier one (e.g. \x1b[22m cancels faint but not
// color). For our use (thinking / tool-output \x1b[2m paired with \x1b[0m)
// this is exact.
static void xline_track_sgr(stringbuf_t *dst, const char *data, ssize_t len) {
  if (dst == NULL) return;
  ssize_t i = 0;
  while (i < len) {
    if ((unsigned char)data[i] != 0x1b) { i++; continue; }
    if (i + 1 >= len || data[i + 1] != '[') { i++; continue; }
    ssize_t start = i;
    ssize_t j     = i + 2;
    // parameter bytes 0x30-0x3f
    while (j < len) {
      unsigned char c = (unsigned char)data[j];
      if (c >= 0x30 && c <= 0x3f) j++; else break;
    }
    // intermediate bytes 0x20-0x2f
    while (j < len) {
      unsigned char c = (unsigned char)data[j];
      if (c >= 0x20 && c <= 0x2f) j++; else break;
    }
    if (j >= len) break; // malformed, stop scanning
    char    final = data[j];
    ssize_t end   = j + 1; // one past the final byte
    if (final == 'm') {
      // Detect reset: parameter bytes are in [start+2, j). A sequence with
      // no parameters (\x1b[m) or one whose params reduce to 0 (e.g.
      // \x1b[0m, \x1b[00m, \x1b[0;0m) clears the stack. Anything else is
      // appended as-is; compound resets embedded in longer sequences are
      // rare enough that we don't bother parsing them.
      int is_reset = 1;
      for (ssize_t k = start + 2; k < j; k++) {
        char c = data[k];
        if (c == '0' || c == ';') continue;
        is_reset = 0;
        break;
      }
      if (is_reset) {
        sbuf_clear(dst);
      } else {
        sbuf_append_n(dst, data + start, end - start);
      }
    }
    i = end;
  }
}

// Emit `data` (len bytes) verbatim into the above region, then repaint the
// edit region underneath. `data` may contain embedded '\n's; any bytes
// following the final '\n' become the new trailing line. When `data` ends
// exactly on '\n' the trailing line is empty and no separator is needed.
static void xline_emit_bytes(xLineHandle_ *h, const char *data, ssize_t len) {
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  xline_trace("emit_bytes: len=%zd", len);
  if (len > 0) xline_trace_bytes("emit_bytes/data", data, len);
  if (len > 0) {
    term_write_n(env->term, data, len);
    // Track cumulative SGR so slow-path rewrites and sneak fast-path
    // replays always see the live styling stack, even after the defining
    // escape has scrolled out of last_line.
    xline_track_sgr(h->active_sgr, data, len);
  }

  // Find last '\n' to split "flushed lines" vs "new trailing line".
  ssize_t last_nl = -1;
  for (ssize_t i = len - 1; i >= 0; i--) {
    if (data[i] == '\n') { last_nl = i; break; }
  }
  sbuf_clear(h->last_line);
  if (last_nl + 1 < len) {
    sbuf_append_n(h->last_line, data + last_nl + 1, len - last_nl - 1);
  }
  h->last_line_rows =
    xline_count_rows(env, sbuf_string(h->last_line), sbuf_len(h->last_line));
  h->last_line_col = 0;
  if (h->last_line_rows > 0) {
    (void)xline_count_rows_and_last_col(env, sbuf_string(h->last_line),
                                        sbuf_len(h->last_line),
                                        &h->last_line_col);
  }

  // Separator '\n' so edit_refresh can safely draw the prompt at column 0
  // of a fresh row. We'll undo this separator on the next flush.
  if (h->last_line_rows > 0) term_write(env->term, "\n");

  // Note: eb->extra is refreshed by the env->refresh_prepare hook we
  // registered in xLineBegin — edit_refresh() will call it internally
  // before it reads extra, so the below panel re-materialises even after
  // completion menus / history search have clobbered the slot.

  edit_refresh(env, eb);
  term_flush(env->term);
}

//-------------------------------------------------------------
// Begin / End
//-------------------------------------------------------------

ic_public xLineHandle xLineBegin(const char *prompt_text) {
  xline_trace("===== xLineBegin(prompt=%s) =====", prompt_text ? prompt_text : "");
  if (g_live_session != NULL) {
  XDEBUG("xline: xLineBegin() called while another session is live\n");
    return NULL;
  }
  ic_env_t *env = ic_get_env();
  if (env == NULL) return NULL;
  if (env->noedit) {
    // dumb terminal / pipe: async editing not supported
    return NULL;
  }

  xLineHandle_ *h = (xLineHandle_ *)calloc(1, sizeof(*h));
  if (h == NULL) return NULL;
  h->env   = env;
  h->state = XLINE_ASYNC_INIT;

  if (!edit_init(env, &h->eb, prompt_text)) {
    free(h);
    return NULL;
  }

  tty_start_raw(env->tty);
  term_start_raw(env->term);

  // Enable bracketed paste (DECSET ?2004). Paired with the DECRST in
  // xLineEnd. xterm-compatible terminals will now wrap pasted text in
  // CSI 200 ~ ... CSI 201 ~, which async.c decodes into PASTE_BEGIN /
  // PASTE_END events. Terminals that don't understand the sequence
  // silently ignore it (it's a standard CSI "unknown private mode"
  // no-op), so there's no fallback branch needed.
  term_write(env->term, "\x1b[?2004h");

  h->last_line      = sbuf_new();
  h->last_line_rows = 0;
  h->last_line_col  = 0;
  h->active_sgr     = sbuf_new();
  h->below_title    = sbuf_new();
  h->below_body     = sbuf_new();
  h->last_term_w    = term_get_width(env->term);
  h->last_term_h    = term_get_height(env->term);

  // Register our refresh hook so every edit_refresh re-injects the
  // below panel (survives completion menus / history search).
  env->refresh_prepare     = &xline_refresh_prepare;
  env->refresh_prepare_arg = h;

  // Force a refresh+flush now so the prompt actually appears on screen
  // before we hand control back to the event loop. The synchronous readline
  // loop doesn't need this because it blocks on input immediately, but we
  // yield to the caller right after xLineBegin. edit_init already queued
  // a prompt copy via edit_write_prompt — edit_refresh below repaints it,
  // term_flush commits both to the tty in order.
  edit_refresh(env, &h->eb);
  term_flush(env->term);

  h->state          = XLINE_ASYNC_RUNNING;
  g_live_session    = h;
  return (xLineHandle)h;
}

ic_public void xLineEnd(xLineHandle handle) {
  if (handle == NULL) return;
  xLineHandle_ *h   = (xLineHandle_ *)handle;
  ic_env_t     *env = h->env;
  xline_trace("===== xLineEnd cur_row=%zd cur_rows=%zd =====",
              (ssize_t)h->eb.cur_row, (ssize_t)h->eb.cur_rows);

  // Release streaming state. The final cooked-mode term_writeln below
  // advances to a fresh line, so even if last_line had unterminated bytes
  // on screen, subsequent output won't glue onto them.
  if (h->last_line != NULL) {
    sbuf_free(h->last_line);
    h->last_line      = NULL;
    h->last_line_rows = 0;
    h->last_line_col  = 0;
  }
  if (h->active_sgr != NULL) {
    sbuf_free(h->active_sgr);
    h->active_sgr = NULL;
  }
  if (h->below_title != NULL) {
    sbuf_free(h->below_title);
    h->below_title = NULL;
  }
  if (h->below_body != NULL) {
    sbuf_free(h->below_body);
    h->below_body = NULL;
  }

  // If finalize hasn't happened yet (user cancelled mid-edit), run it now with
  // KEY_EVENT_STOP so the editor is disposed and history stays consistent.
  if (h->state == XLINE_ASYNC_RUNNING || h->state == XLINE_ASYNC_INIT) {
    char *leftover = edit_finalize(env, &h->eb, KEY_EVENT_STOP);
    if (leftover != NULL) free(leftover);
  } else if (h->taken_line == NULL && h->state == XLINE_ASYNC_DONE_LINE) {
    // Finalize ran but caller never called xLineTake: the line string is
    // owned by edit_finalize's returned heap pointer, which we stored in
    // last_code... wait — we stored in taken_line. If it's NULL here it
    // means finalize returned NULL (EOF-like) and there's nothing to free.
  }
  if (h->taken_line != NULL) {
    // caller forgot to xLineFree the line we handed them? No — taken_line
    // is populated when finalize ran but caller hadn't called Take yet.
    // Free it on their behalf.
    free(h->taken_line);
    h->taken_line = NULL;
  }

  // Unhook ourselves before tearing down — otherwise a late refresh
  // (unlikely, but possible during term_end_raw) would dereference a
  // freed handle.
  if (env->refresh_prepare_arg == h) {
    env->refresh_prepare     = NULL;
    env->refresh_prepare_arg = NULL;
  }

  // Wipe the edit region (the "> <input>" echo that edit_finalize's last
  // edit_refresh left on screen) before dropping raw mode. After this
  // call the cursor sits exactly where the last above-region content
  // ended — i.e. the natural continuation point for the next cooked-mode
  // transcript line.
  //
  // Why wipe instead of leaving the echo in place + writeln:
  //   ai_session's transcript flow re-emits the submitted line via
  //   above_printf("> %s", s) on the *next* xline session so it becomes
  //   a first-class above-region row (same chronological order as
  //   streaming model output, scrolls into scrollback cleanly). If we
  //   left the edit_refresh echo on screen and added a newline, every
  //   submit would show the echo twice: once from xline's edit_refresh,
  //   once from above_printf.
  xline_wipe_edit_region(env, &h->eb);
  term_flush(env->term);

  // Disable bracketed paste before we drop raw mode. Leaving ?2004 on
  // would be harmless functionally, but a well-behaved library always
  // restores the modes it touched.
  term_write(env->term, "\x1b[?2004l");
  term_flush(env->term);

  term_end_raw(env->term, false);
  tty_end_raw(env->tty);

  if (g_live_session == h) g_live_session = NULL;
  free(h);
}

//-------------------------------------------------------------
// Fd accessor
//-------------------------------------------------------------

ic_public int xLineFd(xLineHandle handle) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || h->env == NULL || h->env->tty == NULL) return -1;
#if defined(_WIN32)
  // Windows console input is a HANDLE, not a selectable fd.
  return -1;
#else
  return tty_fd(h->env->tty);
#endif
}

//-------------------------------------------------------------
// Step
//-------------------------------------------------------------

// Advance the edit session by consuming whatever key codes are currently
// buffered. Returns once the tty reports PENDING (no more data right now)
// or a terminal condition is reached.
ic_public xLineStepResult xLineStep(xLineHandle handle) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL) return XLINE_STEP_ERROR;
  switch (h->state) {
  case XLINE_ASYNC_INIT:
  case XLINE_ASYNC_RUNNING:
    break;
  case XLINE_ASYNC_DONE_LINE:
    return XLINE_STEP_LINE;
  case XLINE_ASYNC_DONE_EOF:
    return XLINE_STEP_EOF;
  case XLINE_ASYNC_DONE_INTERRUPT:
    return XLINE_STEP_INTERRUPT;
  case XLINE_ASYNC_DONE_ERROR:
    return XLINE_STEP_ERROR;
  }

  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;

#if !defined(_WIN32)
  {
    int navail = -1;
    int fd     = tty_fd(env->tty);
    if (fd >= 0 && ioctl(fd, FIONREAD, &navail) != 0) navail = -2;
    xline_trace("step: enter state=%d fionread=%d", (int)h->state, navail);
  }
#endif

  // Drain policy: xLineStep is driven by an edge-triggered event loop
  // (kqueue EV_CLEAR / epoll EPOLLET). The loop only re-fires when *new*
  // bytes arrive on the tty fd, so if we return PENDING while bytes are
  // still buffered in the kernel — typical for IME commits and paste,
  // where a single read-ready event delivers many bytes at once — those
  // bytes sit in the kernel buffer indefinitely (edge won't re-fire
  // without fresh writes) until the user happens to press another key.
  // We must therefore drain the tty fully every time we're invoked,
  // returning PENDING only once tty_read_timeout reports no input left.
  //
  // Starvation of other fds isn't a practical concern here: tty input
  // rate is bounded by human typing / a single paste buffer, and the
  // outer loop re-enters on each poll cycle anyway.
  for (;;) {
    code_t c;
    // Non-blocking read: tty_read_timeout(_, 0, _) may still briefly block
    // up to the ESC follow-up timeout (~10ms) while reassembling a partial
    // escape sequence — this is short enough to be acceptable in an event
    // loop.
    term_flush(env->term);

    // Pick up SIGWINCH between key dispatches. edit_resize recomputes the
    // edit region's layout against the new geometry. tty_term_resize_event()
    // cannot distinguish a real geometry change from a spurious SIGWINCH
    // (focus/theme events in some terminal emulators) or, on platforms
    // without a native resize event, simply returns true every time. So we
    // gate the expensive edit_resize on an actual change in columns/rows.
    if (tty_term_resize_event(env->tty)) {
      ssize_t nw = term_get_width(env->term);
      ssize_t nh = term_get_height(env->term);
      if (nw != h->last_term_w || nh != h->last_term_h) {
        xline_trace("resize event (real): %zdx%zd -> %zdx%zd",
                    h->last_term_w, h->last_term_h, nw, nh);
        edit_resize(env, eb);
        h->last_term_w = nw;
        h->last_term_h = nh;
      } else {
        xline_trace("resize event (spurious): %zdx%zd unchanged", nw, nh);
      }
    }

    if (!tty_read_timeout(env->tty, 0, &c)) {
#if !defined(_WIN32)
      {
        int navail = -1;
        int fd     = tty_fd(env->tty);
        if (fd >= 0 && ioctl(fd, FIONREAD, &navail) != 0) navail = -2;
        xline_trace("step: exit PENDING fionread=%d", navail);
      }
#endif
      return XLINE_STEP_PENDING;
    }
    xline_trace("step: got key code=0x%x", (unsigned)c);

    // Bracketed paste boundary handling. We must intercept these
    // BEFORE edit_dispatch_key: PASTE_BEGIN flips us into a mode where
    // incoming bytes bypass KEY_ENTER's "submit" semantics and get
    // inserted literally (with \r normalised to \n), and PASTE_END
    // restores normal dispatch plus issues the one coalesced refresh.
    if (c == KEY_EVENT_PASTE_BEGIN) {
      xline_trace("paste: begin");
      h->in_paste        = true;
      eb->suspend_refresh = true;
      continue;
    }
    if (c == KEY_EVENT_PASTE_END) {
      xline_trace("paste: end input_len=%zd", sbuf_len(eb->input));
      h->in_paste        = false;
      eb->suspend_refresh = false;
      // Single coalesced repaint for the whole paste. Using
      // edit_refresh (not _hint) is deliberate: we don't want the
      // post-paste frame to auto-pop a completion hint based on the
      // now-huge buffer — hints are a typing-affordance and fire
      // naturally on the next real keystroke.
      edit_refresh(env, eb);
      continue;
    }
    if (h->in_paste) {
      // Literal insertion path. Accept anything that represents a
      // character; reject control keys and events (arrows etc. should
      // never appear inside a paste but be defensive). \r is
      // normalised to \n so multi-line pastes look sane regardless of
      // whether the terminal sent CR, LF or CRLF line endings.
      char      chr;
      unicode_t uchr;
      if (c == KEY_ENTER /* '\r' */ || c == KEY_LINEFEED /* '\n' */) {
        edit_insert_char(env, eb, '\n');
      } else if (c == KEY_TAB) {
        edit_insert_char(env, eb, '\t');
      } else if (code_is_ascii_char(c, &chr)) {
        edit_insert_char(env, eb, chr);
      } else if (code_is_unicode(c, &uchr)) {
        edit_insert_unicode(env, eb, uchr);
      } else {
        xline_trace("paste: drop non-char code=0x%x", (unsigned)c);
      }
      continue;
    }

    if (edit_dispatch_key(env, eb, c)) {
      // terminal condition: finalize and record the outcome
      h->last_code  = c;
      h->taken_line = edit_finalize(env, eb, c);

      // Ctrl-C / ^G: edit.c wipes the buffer via edit_delete_all and
      // asks us to finalise, but edit_finalize() only maps to NULL
      // for Ctrl-D / STOP — a blanked buffer comes back as a freshly
      // strdup'd "" and would otherwise look indistinguishable from
      // "user pressed Enter on an empty line". Detect it here, drop
      // the empty string, and surface an explicit INTERRUPT so the
      // application layer can tell "abort current work" apart from
      // "idle Enter" or "EOF".
      if (c == KEY_CTRL_C || c == KEY_BELL /* ^G */) {
        if (h->taken_line != NULL) {
          free(h->taken_line);
          h->taken_line = NULL;
        }
        h->state = XLINE_ASYNC_DONE_INTERRUPT;
        return XLINE_STEP_INTERRUPT;
      }

      if (h->taken_line == NULL) {
        // Ctrl-D on empty input, KEY_EVENT_STOP, ESC on empty input —
        // historical EOF mapping.
        h->state = XLINE_ASYNC_DONE_EOF;
        return XLINE_STEP_EOF;
      }
      h->state = XLINE_ASYNC_DONE_LINE;
      return XLINE_STEP_LINE;
    }
  }
}

//-------------------------------------------------------------
// Take (transfer line ownership to caller)
//-------------------------------------------------------------

ic_public char *xLineTake(xLineHandle handle) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || h->state != XLINE_ASYNC_DONE_LINE) return NULL;
  char *line    = h->taken_line;
  h->taken_line = NULL;
  return line;
}

//-------------------------------------------------------------
// Print above the current edit line (token-level streaming).
//
// Both entry points go through xline_erase_trailing() to wipe the edit
// region *and* the previous trailing line, then xline_emit_bytes() writes
// the new bytes, records the new trailing line and repaints the prompt.
// See the XDEF_STRUCT comments above for the invariant and state.
//-------------------------------------------------------------

ic_public void xLinePrintAbove(xLineHandle handle, const char *s) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || s == NULL || h->env == NULL) return;
  if (h->state != XLINE_ASYNC_RUNNING && h->state != XLINE_ASYNC_INIT) return;

  // xLinePrintAbove semantics: `s` is a complete chunk that should close out
  // the current trailing line. Append `s`, then a '\n' if it isn't already
  // one, so the whole lot becomes "flushed lines" and the new trailing line
  // is empty.
  //
  // We also prepend the currently-active SGR stack so that a caller whose
  // styling was set on a scrolled-out line still influences this emission.
  // xline_emit_bytes rebuilds active_sgr from the data it's handed, so we
  // clear it first to avoid double-tracking the prefix we prepend.
  stringbuf_t *buf = sbuf_new();
  if (buf == NULL) return;
  if (h->active_sgr != NULL && sbuf_len(h->active_sgr) > 0) {
    sbuf_append(buf, sbuf_string(h->active_sgr));
    sbuf_clear(h->active_sgr);
  }
  sbuf_append(buf, sbuf_string(h->last_line));
  sbuf_append(buf, s);
  ssize_t blen = sbuf_len(buf);
  if (blen == 0 || sbuf_string(buf)[blen - 1] != '\n') {
    sbuf_append_char(buf, '\n');
  }

  xline_erase_trailing(h);
  xline_emit_bytes(h, sbuf_string(buf), sbuf_len(buf));
  sbuf_free(buf);
}

ic_public void xLinePrintAboveChunk(xLineHandle handle, const char *s) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || s == NULL || *s == '\0' || h->env == NULL) return;
  if (h->state != XLINE_ASYNC_RUNNING && h->state != XLINE_ASYNC_INIT) return;

  // Fast path (token-streaming sneak): the common case for LLM streaming
  // is one chunk with no embedded '\n'. Previously every such chunk
  // triggered the full erase_trailing + emit_bytes + edit_refresh cycle,
  // which rewrites the *entire* trailing line and the prompt for every
  // token. For long unbroken replies (tables, prose paragraphs without
  // blank lines) last_line grows without bound between '\n's, so that
  // rewrite becomes O(N²) in total bytes — and on real LLM streams the
  // per-tick cost visibly outruns the input-handling budget, producing
  // the classic "output freezes, keys don't echo" symptom.
  //
  // We therefore sneak on ANY no-newline chunk. Two sub-cases:
  //
  //   A. The chunk extends last_line horizontally only — the terminal's
  //      cell count for (last_line ++ chunk) still fits in the same
  //      number of screen rows. We position the cursor at the current
  //      tail (no wipe), write the chunk, restore to the edit region.
  //      Zero repaint of last_line, zero repaint of the edit region.
  //
  //   B. The chunk pushes last_line onto one or more new screen rows.
  //      We still don't rewrite last_line — we just write the chunk at
  //      the tail; the terminal handles autowrap into the fresh rows
  //      itself. The edit region below does get shoved down by those
  //      new rows (autowrap scrolls it), so we bump last_line_rows and
  //      issue a single edit_refresh to redraw the prompt in its new
  //      position. That's the same shape as sub-case A but with the
  //      rows bookkeeping updated + one refresh, which is still vastly
  //      cheaper than the slow path's "re-emit the entire trailing
  //      line from scratch" every tick.
  //
  // The slow path below is now reserved for chunks that actually contain
  // a '\n' (the only shape that truly forces a boundary re-layout).
  if (strchr(s, '\n') == NULL && h->last_line_rows > 0) {
    ic_env_t *env = h->env;
    editor_t *eb  = &h->eb;
    // Use the cached column from the previous successful sneak/emit
    // instead of re-scanning the entire last_line (which is O(N) per
    // chunk and makes long runs O(N²) total).
    ssize_t last_col = h->last_line_col;

    // Compute new rows/col from the chunk *alone*, starting at the
    // cached last_col as the initial column offset.  This turns the
    // per-chunk work from O(last_line_len) into O(chunk_len).
    ssize_t chunk_col = last_col;
    ssize_t chunk_rows = 0;
    {
      ssize_t termw = term_get_width(env->term);
      if (termw <= 0) termw = 80;
      chunk_rows = xline_count_rows_from_col(env, s, (ssize_t)strlen(s),
                                             termw, chunk_col, &chunk_col);
    }
    ssize_t new_rows     = h->last_line_rows + chunk_rows;
    ssize_t new_last_col = chunk_col;

    if (new_rows >= h->last_line_rows) {
      // Sneak path is viable (both sub-cases).
      ssize_t added_rows = new_rows - h->last_line_rows;
      xline_trace("sneak: last_rows=%zd+%zd last_col=%zd -> col=%zd "
                  "chunk_len=%zu",
                  h->last_line_rows, added_rows, last_col, new_last_col,
                  (size_t)strlen(s));
      term_attr_reset(env->term);
      term_write(env->term, "\x1b" "7"); // save cursor (DECSC)
      // Climb from current edit-region cursor to the final row of
      // last_line. cur_row is 0-based inside the edit region; one
      // extra row up accounts for the '\n' separator between the
      // above region and the prompt.
      term_up(env->term, (ssize_t)eb->cur_row + 1);
      term_write(env->term, "\r");
      term_right(env->term, last_col);
      // Re-establish the active SGR stack before writing the chunk, so
      // styling set by an earlier above_chunk survives the term_attr_reset
      // above — even if the defining escape has since scrolled out of
      // last_line (e.g. \x1b[2m on row 0, then a '\n' followed by more
      // thinking text on row 1+). active_sgr is kept in sync by
      // xline_emit_bytes (slow path) and xline_track_sgr below (fast
      // path). Zero-width, so cursor position set up by term_up/
      // term_right stays valid; DECSC above saved the default attrs, so
      // DECRC below still restores the edit region to a clean state.
      if (h->active_sgr != NULL && sbuf_len(h->active_sgr) > 0) {
        term_write_n(env->term, sbuf_string(h->active_sgr),
                     sbuf_len(h->active_sgr));
      }
      term_write(env->term, s);
      if (added_rows == 0) {
        // Sub-case A: no new rows, DECRC restores the edit region
        // cursor exactly.
        term_write(env->term, "\x1b" "8"); // restore cursor (DECRC)
        term_flush(env->term);
      } else {
        // Sub-case B: last_line now occupies `added_rows` more screen
        // rows than before. The autowrap from `term_write(s)` already
        // pushed the edit region down by that many rows — DECRC would
        // restore the cursor to the *old* edit-region position, which
        // now sits on top of the extended last_line. Discard DECRC
        // and redraw the edit region in its new position instead.
        term_flush(env->term);
        h->last_line_rows = new_rows;
        edit_refresh(env, eb);
      }
      // Commit the new trailing line.
      sbuf_append(h->last_line, s);
      h->last_line_col = new_last_col;
      // Keep active_sgr up to date with what the chunk just emitted:
      // the slow path reads from it, and the next sneak tick replays it.
      xline_track_sgr(h->active_sgr, s, (ssize_t)strlen(s));
      return;
    }
  }

  // Slow path: extend the trailing line with the caller's fragment. We need
  // to rewrite the previous trailing line too (it may now wrap differently,
  // or may be fully consumed by a '\n' inside `s`), so concatenate and
  // re-emit. Prepend the active SGR stack so styling set on a now-scrolled
  // line is re-armed before the re-emission; clear active_sgr because
  // xline_emit_bytes will rebuild it from the exact bytes we hand it.
  stringbuf_t *buf = sbuf_new();
  if (buf == NULL) return;
  if (h->active_sgr != NULL && sbuf_len(h->active_sgr) > 0) {
    sbuf_append(buf, sbuf_string(h->active_sgr));
    sbuf_clear(h->active_sgr);
  }
  sbuf_append(buf, sbuf_string(h->last_line));
  sbuf_append(buf, s);

  xline_erase_trailing(h);
  xline_emit_bytes(h, sbuf_string(buf), sbuf_len(buf));
  sbuf_free(buf);
}

//-------------------------------------------------------------
// Below panel control.
//
// The panel content lives in h->below_{title,body}. Rendering is driven
// by xline_sync_extra_from_below(), which is called on every refresh path
// (including xline_emit_bytes above). When the caller mutates the panel
// outside a streaming tick, we need to drive a repaint ourselves — that
// is what xline_repaint_below() does: it re-emits the existing trailing
// line through the same erase/emit cycle, which transitively repaints
// the edit region and the now-updated below panel.
//-------------------------------------------------------------

static void xline_repaint_below(xLineHandle_ *h) {
  if (h->state != XLINE_ASYNC_RUNNING && h->state != XLINE_ASYNC_INIT) return;
  /* No trailing line and no panel contents means nothing on screen
   * would change — avoid the wipe+refresh cycle so callers that clear
   * an already-empty panel don't cost a redundant repaint. */
  if (sbuf_len(h->last_line) == 0 &&
      sbuf_len(h->below_title) == 0 &&
      sbuf_len(h->below_body) == 0) {
    return;
  }
  stringbuf_t *buf = sbuf_new();
  if (buf == NULL) return;
  sbuf_append(buf, sbuf_string(h->last_line));
  xline_erase_trailing(h);
  xline_emit_bytes(h, sbuf_string(buf), sbuf_len(buf));
  sbuf_free(buf);
}

ic_public void xLineSetBelowPanel(xLineHandle handle, const char *title,
                                  const char *body) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || h->env == NULL) return;
  if (h->below_title == NULL || h->below_body == NULL) return;

  sbuf_clear(h->below_title);
  if (title != NULL && *title != '\0') sbuf_append(h->below_title, title);

  sbuf_clear(h->below_body);
  if (body != NULL && *body != '\0') {
    // Strip a trailing '\n' so the panel's bottom row doesn't eat an extra
    // blank line below itself.
    ssize_t n = (ssize_t)strlen(body);
    if (n > 0 && body[n - 1] == '\n') n--;
    sbuf_append_n(h->below_body, body, n);
  }

  xline_repaint_below(h);
}

ic_public void xLineClearBelowPanel(xLineHandle handle) {
  xLineHandle_ *h = (xLineHandle_ *)handle;
  if (h == NULL || h->env == NULL) return;
  if (h->below_title == NULL || h->below_body == NULL) return;

  sbuf_clear(h->below_title);
  sbuf_clear(h->below_body);
  xline_repaint_below(h);
}
