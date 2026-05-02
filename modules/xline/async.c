/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen (original isocline)
  Copyright (c) 2026, xKit contributors (xline async additions)
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE.isocline" file at the root of this module.
-----------------------------------------------------------------------------*/
//-------------------------------------------------------------
// FD-level async line editing.
//
// Relies on the synchronous edit primitives declared in editline.h
// (editor_t, edit_init, edit_dispatch_key, edit_finalize, edit_refresh,
// edit_write_prompt) and tty.c (tty_read_timeout, tty_start_raw,
// tty_end_raw, tty_fd).
//-------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include <xbase/log.h>
#include "editline.h"
#include "env.h"
#include "line.h"
#include "mem.h"
#include "platform.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"

//-------------------------------------------------------------
// Handle state
//-------------------------------------------------------------

typedef enum xline_async_state_e {
  XLINE_ASYNC_INIT = 0,   // freshly begun; Begin/Step not yet taken/ended
  XLINE_ASYNC_RUNNING,    // still feeding keys
  XLINE_ASYNC_DONE_LINE,  // user pressed Enter (or cancel returning empty
                          // string)
  XLINE_ASYNC_DONE_EOF,   // Ctrl-D on empty input or STOP
  XLINE_ASYNC_DONE_ERROR, // unrecoverable error
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
};

// Only one live session at a time (requirement 4.4).
static xLineHandle_ *g_live_session = NULL;

// Exposed (internally) so the synchronous readline path can assert non-reentry.
ic_private bool xline_async_is_live(void) {
  return g_live_session != NULL;
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
static ssize_t xline_count_rows(ic_env_t *env, const char *s, ssize_t len) {
  if (len <= 0) return 0;
  stringbuf_t *tmp = sbuf_new(env->mem);
  if (tmp == NULL) return 1; // best-effort fallback
  sbuf_append_n(tmp, s, len);
  ssize_t  termw = term_get_width(env->term);
  rowcol_t rc    = {0};
  ssize_t  rows  = sbuf_get_rc_at_pos(tmp, termw, 0, 0, sbuf_len(tmp), &rc);
  sbuf_free(tmp);
  return rows;
}

// Rewind the cursor to the start of the above region's trailing line and
// clear from there to the end of the screen. After this call the terminal
// write head is positioned exactly where the next above-region byte should
// land. `h->last_line_rows` is the number of screen rows occupied by the
// old trailing line; the separator '\n' that follows it is one extra row.
static void xline_erase_trailing(xLineHandle_ *h) {
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  xline_wipe_edit_region(env, eb);
  if (h->last_line_rows > 0) {
    // edit_refresh landed the prompt on the line *after* last_line; we've
    // just walked up onto that separator line. Step up past last_line's
    // rows too.
    term_up(env->term, h->last_line_rows);
    term_write(env->term, "\r\x1b[J");
  }
}

// Emit `data` (len bytes) verbatim into the above region, then repaint the
// edit region underneath. `data` may contain embedded '\n's; any bytes
// following the final '\n' become the new trailing line. When `data` ends
// exactly on '\n' the trailing line is empty and no separator is needed.
static void xline_emit_bytes(xLineHandle_ *h, const char *data, ssize_t len) {
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  if (len > 0) term_write_n(env->term, data, len);

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

  // Separator '\n' so edit_refresh can safely draw the prompt at column 0
  // of a fresh row. We'll undo this separator on the next flush.
  if (h->last_line_rows > 0) term_write(env->term, "\n");

  edit_refresh(env, eb);
  term_flush(env->term);
}

//-------------------------------------------------------------
// Begin / End
//-------------------------------------------------------------

ic_public xLineHandle xLineBegin(const char *prompt_text) {
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

  xLineHandle_ *h = (xLineHandle_ *)mem_zalloc(env->mem, sizeof(*h));
  if (h == NULL) return NULL;
  h->env   = env;
  h->state = XLINE_ASYNC_INIT;

  if (!edit_init(env, &h->eb, prompt_text)) {
    mem_free(env->mem, h);
    return NULL;
  }

  tty_start_raw(env->tty);
  term_start_raw(env->term);

  // edit_init painted the prompt into the term buffer via edit_write_prompt,
  // but didn't flush — and unlike the synchronous readline loop we don't
  // immediately block on input to drain it. Force a refresh+flush now so the
  // prompt actually appears on screen before we hand control back to the
  // event loop.
  edit_refresh(env, &h->eb);
  term_flush(env->term);

  h->state          = XLINE_ASYNC_RUNNING;
  h->last_line      = sbuf_new(env->mem);
  h->last_line_rows = 0;
  g_live_session    = h;
  return (xLineHandle)h;
}

ic_public void xLineEnd(xLineHandle handle) {
  if (handle == NULL) return;
  xLineHandle_ *h   = (xLineHandle_ *)handle;
  ic_env_t     *env = h->env;

  // Release streaming state. The final cooked-mode term_writeln below
  // advances to a fresh line, so even if last_line had unterminated bytes
  // on screen, subsequent output won't glue onto them.
  if (h->last_line != NULL) {
    sbuf_free(h->last_line);
    h->last_line      = NULL;
    h->last_line_rows = 0;
  }

  // If finalize hasn't happened yet (user cancelled mid-edit), run it now with
  // KEY_EVENT_STOP so the editor is disposed and history stays consistent.
  if (h->state == XLINE_ASYNC_RUNNING || h->state == XLINE_ASYNC_INIT) {
    char *leftover = edit_finalize(env, &h->eb, KEY_EVENT_STOP);
    if (leftover != NULL) mem_free(env->mem, leftover);
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
    mem_free(env->mem, h->taken_line);
    h->taken_line = NULL;
  }

  term_end_raw(env->term, false);
  tty_end_raw(env->tty);
  term_writeln(env->term, "");
  term_flush(env->term);

  if (g_live_session == h) g_live_session = NULL;
  mem_free(env->mem, h);
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
  case XLINE_ASYNC_DONE_ERROR:
    return XLINE_STEP_ERROR;
  }

  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;

  for (;;) {
    code_t c;
    // Non-blocking read: drain whatever is ready on the tty right now.
    // tty_read_timeout(_, 0, _) may still briefly block up to the ESC
    // follow-up timeout (~10ms) while reassembling a partial escape
    // sequence — this is short enough to be acceptable in an event loop.
    term_flush(env->term);
    if (!tty_read_timeout(env->tty, 0, &c)) {
      return XLINE_STEP_PENDING;
    }
    if (edit_dispatch_key(env, eb, c)) {
      // terminal condition: finalize and record the outcome
      h->last_code  = c;
      h->taken_line = edit_finalize(env, eb, c);
      if (h->taken_line == NULL) {
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
  stringbuf_t *buf = sbuf_new(h->env->mem);
  if (buf == NULL) return;
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

  // Extend the trailing line with the caller's fragment. We need to rewrite
  // the previous trailing line too (it may now wrap differently, or may be
  // fully consumed by a '\n' inside `s`), so concatenate and re-emit.
  stringbuf_t *buf = sbuf_new(h->env->mem);
  if (buf == NULL) return;
  sbuf_append(buf, sbuf_string(h->last_line));
  sbuf_append(buf, s);

  xline_erase_trailing(h);
  xline_emit_bytes(h, sbuf_string(buf), sbuf_len(buf));
  sbuf_free(buf);
}
