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

struct xLineHandle_s {
  ic_env_t           *env;
  editor_t            eb;
  xline_async_state_t state;
  code_t last_code;  // last code that drove us into a terminal state
  char  *taken_line; // cached result after finalize
};

// Only one live session at a time (requirement 4.4).
static xLineHandle *g_live_session = NULL;

// Exposed (internally) so the synchronous readline path can assert non-reentry.
ic_private bool xline_async_is_live(void) {
  return g_live_session != NULL;
}

//-------------------------------------------------------------
// Begin / End
//-------------------------------------------------------------

ic_public xLineHandle *xLineBegin(const char *prompt_text) {
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

  xLineHandle *h = (xLineHandle *)mem_zalloc(env->mem, sizeof(*h));
  if (h == NULL) return NULL;
  h->env   = env;
  h->state = XLINE_ASYNC_INIT;

  if (!edit_init(env, &h->eb, prompt_text)) {
    mem_free(env->mem, h);
    return NULL;
  }

  tty_start_raw(env->tty);
  term_start_raw(env->term);
  term_flush(env->term);

  h->state       = XLINE_ASYNC_RUNNING;
  g_live_session = h;
  return h;
}

ic_public void xLineEnd(xLineHandle *h) {
  if (h == NULL) return;
  ic_env_t *env = h->env;

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

ic_public int xLineFd(xLineHandle *h) {
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
ic_public xLineStepResult xLineStep(xLineHandle *h) {
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

ic_public char *xLineTake(xLineHandle *h) {
  if (h == NULL || h->state != XLINE_ASYNC_DONE_LINE) return NULL;
  char *line    = h->taken_line;
  h->taken_line = NULL;
  return line;
}

//-------------------------------------------------------------
// Print above the current edit line
//-------------------------------------------------------------

ic_public void xLinePrintAbove(xLineHandle *h, const char *s) {
  if (h == NULL || s == NULL || h->env == NULL) return;
  if (h->state != XLINE_ASYNC_RUNNING && h->state != XLINE_ASYNC_INIT) return;
  ic_env_t *env = h->env;
  editor_t *eb  = &h->eb;
  // Move cursor to column 0, erase everything that the current edit session
  // has painted (prompt + input + any completion menu), then emit the
  // caller's text followed by a newline, and repaint the edit view.
  // edit_clear_screen-style full clear would also work but is heavier and
  // flickers; here we rely on edit_refresh() to redraw after we reset.
  term_write(env->term,
             "\r\x1b[J"); // CSI 0J — clear from cursor to end of screen
  term_write(env->term, s);
  // Ensure the chunk ends on a fresh line so the prompt repaints cleanly.
  size_t len = strlen(s);
  if (len == 0 || s[len - 1] != '\n') {
    term_write(env->term, "\n");
  }
  // Reset row bookkeeping so edit_refresh repaints from here.
  eb->cur_row  = 0;
  eb->cur_rows = 1;
  edit_write_prompt(env, eb, 0, false);
  edit_refresh(env, eb);
  term_flush(env->term);
}
