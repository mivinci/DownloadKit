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
// Relies on the synchronous edit primitives declared in edit.h
// (editor_t, edit_init, edit_dispatch_key, edit_finalize, edit_refresh,
// edit_write_prompt) and tty.c (tty_read_timeout, tty_start_raw,
// tty_end_raw, tty_fd).
//-------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include <xbase/log.h>
#include "edit.h"
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

  // Vertical anchoring — keep the edit region (prompt + input + below
  // panel) glued to the screen's bottom rows.
  //
  // Early design used an ESC[6n (CPR) round-trip on every frame to ask
  // the terminal where the cursor actually is. That cost ~5–20ms per
  // token and caused a visible flicker (the cursor froze at the end of
  // the above bytes while we waited for the response, then snapped back
  // to the prompt after the next refresh).
  //
  // Instead we query CPR exactly once, in xLineBegin, to learn the
  // starting row, and keep a pure-local estimate of where the cursor
  // ends up after each above-region write:
  //
  //   cur_row_after_above = min(start_row + above_flushed_rows +
  //                             last_line_rows, screen_height)
  //
  // The terminal pins the cursor to the bottom row once output would
  // scroll past the screen, so we clamp at `th` to model that. When the
  // edit region would render above `th - edit_rows + 1`, we pad with
  // that many '\n's and remember the count in `anchor_pad` so the next
  // wipe walks past them.
  //
  // `anchor_stuck` latches once above_flushed_rows + last_line_rows +
  // edit_rows >= screen_height: from then on the terminal self-scrolls
  // the above region and anchoring is a no-op. SIGWINCH clears it so we
  // re-measure against the new geometry; `start_row` is re-queried on
  // the next paint via a lazy bool.
  ssize_t             anchor_pad;
  bool                anchor_stuck;
  // Cumulative count of '\n' bytes committed to the above region since
  // start_row was sampled (not counting anchor pad).
  ssize_t             above_flushed_rows;
  // Screen row (1-based) where the above region's top sits, as returned
  // by the one-shot CPR query. 0 means "not yet sampled"; the next
  // anchor call will sample it.
  ssize_t             start_row;
  bool                start_row_valid;
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

  // Separator line with optional title: "── /help ──" (dim).
  if (h->below_title != NULL && sbuf_len(h->below_title) > 0) {
    sbuf_appendf(eb->extra, "[ic-diminish]\xe2\x94\x80\xe2\x94\x80 [!pre]%s[/pre] \xe2\x94\x80\xe2\x94\x80[/]\n",
                 sbuf_string(h->below_title));
  } else {
    sbuf_append(eb->extra, "[ic-diminish]\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80[/]\n");
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

// Same as xline_wipe_edit_region but also walks up past the anchor
// padding (blank '\n' rows we injected just before the previous paint
// to keep the edit region glued to the screen bottom). After the call
// the cursor sits exactly where the previous paint's above-region
// content ended, ready for the next above chunk to append.
static void xline_wipe_edit_region_and_anchor(xLineHandle_ *h) {
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  xline_wipe_edit_region(env, eb);
  if (h->anchor_pad > 0) {
    term_up(env->term, h->anchor_pad);
    term_write(env->term, "\r\x1b[J");
    h->anchor_pad = 0;
  }
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
  xline_wipe_edit_region_and_anchor(h);
  if (h->last_line_rows > 0) {
    // edit_refresh landed the prompt on the line *after* last_line; we've
    // just walked up onto that separator line. Step up past last_line's
    // rows too.
    term_up(env->term, h->last_line_rows);
    term_write(env->term, "\r\x1b[J");
  }
  (void)eb; // retained for readability; modifications happened inside wipe
}

// Ensure h->start_row is populated. On first call (or after SIGWINCH
// invalidated it), flush the pending term buffer and issue a single
// ESC[6n CPR round-trip. All subsequent anchor decisions are pure
// arithmetic against this anchor point.
static void xline_ensure_start_row(xLineHandle_ *h) {
  if (h->start_row_valid) return;
  ic_env_t *env = h->env;
  term_flush(env->term);
  ssize_t cr = 0;
  if (!term_cursor_row(env->term, &cr) || cr <= 0) {
    // CPR failed. Best-effort fallback: assume we're at the bottom row,
    // which makes anchor a no-op (no pad needed).
    h->start_row       = term_get_height(env->term);
    h->start_row_valid = true;
    h->anchor_stuck    = true;
    return;
  }
  h->start_row       = cr;
  h->start_row_valid = true;
}

// Adjust vertical position of the prompt so the edit region sits flush
// against the screen's bottom rows. Called once per paint right before
// edit_refresh. Entirely local arithmetic — no CPR round-trip, no flush
// — so the hot-path cost is a handful of comparisons.
//
// Limitation: we compute against `eb->cur_rows` as left over by the
// previous refresh. If the input grows across this paint (wrapping
// input line, completion menu opening, etc.) the target is off by a
// row or two on the first frame — the next paint corrects it.
static void xline_anchor_edit_bottom(xLineHandle_ *h) {
  ic_env_t *env = h->env;
  // Caller opted out of bottom-anchoring via xLineEnableAnchor(false).
  // Clear any pad we had injected on a previous frame so the next
  // xline_wipe_edit_region_and_anchor walk doesn't chew into real
  // above-region content, then bail before we compute a new pad.
  if (env->no_anchor) {
    h->anchor_pad = 0;
    return;
  }
  if (h->anchor_stuck) {
    h->anchor_pad = 0;
    return;
  }
  editor_t *eb = &h->eb;
  ssize_t   th = term_get_height(env->term);
  if (th <= 1) return; // unknown height — refuse to guess

  xline_ensure_start_row(h);
  if (h->anchor_stuck) return; // CPR fallback already latched

  ssize_t edit_rows = eb->cur_rows > 0 ? eb->cur_rows : 1;

  // Where would the cursor sit right now, just after the above-region
  // bytes for this paint have been written (including the separator '\n'
  // this emit added)? It's start_row plus every flushed row we've
  // committed so far, plus however many rows last_line currently wraps
  // to. The terminal clamps cursor to the bottom row once output would
  // scroll past the screen, so mirror that clamp.
  ssize_t cr = h->start_row + h->above_flushed_rows + h->last_line_rows;
  if (cr > th) cr = th;
  if (cr < 1)  cr = 1;

  // Fast path: once the real above-region rows have caught up to the
  // screen height, the terminal is self-scrolling the above region
  // into scrollback and we don't need to pad anymore. Latch.
  if (cr + edit_rows - 1 >= th) {
    h->anchor_stuck = true;
    h->anchor_pad   = 0;
    return;
  }

  // Desired top row of the edit region:
  ssize_t want_top = th - edit_rows + 1;
  if (want_top < 1) want_top = 1;

  if (cr < want_top) {
    ssize_t pad = want_top - cr;
    term_write_repeat(env->term, "\n", pad);
    h->anchor_pad = pad;
  } else {
    h->anchor_pad = 0;
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

  // Find last '\n' to split "flushed lines" vs "new trailing line", and
  // accumulate the flushed-line count used by anchor_stuck's self-scroll
  // detection.
  ssize_t last_nl = -1;
  ssize_t nls     = 0;
  for (ssize_t i = len - 1; i >= 0; i--) {
    if (data[i] == '\n') {
      nls++;
      if (last_nl < 0) last_nl = i;
    }
  }
  h->above_flushed_rows += nls;
  sbuf_clear(h->last_line);
  if (last_nl + 1 < len) {
    sbuf_append_n(h->last_line, data + last_nl + 1, len - last_nl - 1);
  }
  h->last_line_rows =
    xline_count_rows(env, sbuf_string(h->last_line), sbuf_len(h->last_line));

  // Separator '\n' so edit_refresh can safely draw the prompt at column 0
  // of a fresh row. We'll undo this separator on the next flush.
  if (h->last_line_rows > 0) term_write(env->term, "\n");

  // Note: eb->extra is refreshed by the env->refresh_prepare hook we
  // registered in xLineBegin — edit_refresh() will call it internally
  // before it reads extra, so the below panel re-materialises even after
  // completion menus / history search have clobbered the slot.

  // Push the prompt down to the bottom of the screen if it would
  // otherwise float mid-screen. After the above region has grown past
  // one screen this is a no-op (anchor_stuck latches).
  xline_anchor_edit_bottom(h);

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

  h->last_line          = sbuf_new(env->mem);
  h->last_line_rows     = 0;
  h->below_title        = sbuf_new(env->mem);
  h->below_body         = sbuf_new(env->mem);
  h->anchor_pad         = 0;
  h->anchor_stuck       = false;
  h->above_flushed_rows = 0;
  h->start_row          = 0;
  h->start_row_valid    = false;

  // Register our refresh hook so every edit_refresh re-injects the
  // below panel (survives completion menus / history search).
  env->refresh_prepare     = &xline_refresh_prepare;
  env->refresh_prepare_arg = h;

  // edit_init already queued a copy of the prompt into the term buffer via
  // edit_write_prompt. Drop it on the floor: we are about to repaint the
  // prompt ourselves from edit_refresh after anchoring the edit region to
  // the bottom of the screen, and a stray pre-anchor copy would otherwise
  // ghost on the starting row once we flush.
  term_discard_buffer(env->term);
  term_attr_reset(env->term); // neutralise any in-flight SGR state

  // Push the prompt down to the bottom of the screen before the very first
  // refresh so the session opens with the edit region glued to the bottom
  // rows instead of floating mid-screen.
  xline_anchor_edit_bottom(h);

  // Force a refresh+flush now so the prompt actually appears on screen
  // before we hand control back to the event loop. The synchronous readline
  // loop doesn't need this because it blocks on input immediately, but we
  // yield to the caller right after xLineBegin.
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

  // Release streaming state. The final cooked-mode term_writeln below
  // advances to a fresh line, so even if last_line had unterminated bytes
  // on screen, subsequent output won't glue onto them.
  if (h->last_line != NULL) {
    sbuf_free(h->last_line);
    h->last_line      = NULL;
    h->last_line_rows = 0;
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

  // Unhook ourselves before tearing down — otherwise a late refresh
  // (unlikely, but possible during term_end_raw) would dereference a
  // freed handle.
  if (env->refresh_prepare_arg == h) {
    env->refresh_prepare     = NULL;
    env->refresh_prepare_arg = NULL;
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
  case XLINE_ASYNC_DONE_INTERRUPT:
    return XLINE_STEP_INTERRUPT;
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

    // Pick up SIGWINCH between key dispatches. edit_resize recomputes the
    // edit region's layout against the new geometry; we also clear the
    // anchor latch so the next paint re-measures against the new screen
    // height. Any pad rows we had injected on the old screen were
    // invalidated by the resize and will be refreshed out on the next
    // emit.
    if (tty_term_resize_event(env->tty)) {
      edit_resize(env, eb);
      // Screen geometry changed. Drop all cached anchor state so the
      // next paint re-samples the cursor row via CPR once, then
      // proceeds entirely with local arithmetic again.
      h->anchor_pad         = 0;
      h->anchor_stuck       = false;
      h->above_flushed_rows = 0;
      h->start_row_valid    = false;
    }

    if (!tty_read_timeout(env->tty, 0, &c)) {
      return XLINE_STEP_PENDING;
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
          mem_free(env->mem, h->taken_line);
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
  stringbuf_t *buf = sbuf_new(h->env->mem);
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
