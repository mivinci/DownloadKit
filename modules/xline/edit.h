/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef IC_EDIT_H
#define IC_EDIT_H

#include "attr.h"
#include "platform.h"
#include "stringbuf.h"
#include "tty.h"
#include "undo.h"
#include "unicode.h"

//-------------------------------------------------------------
// The editor state
//
// Lifted from edit.c so that other internal translation units
// (e.g. async.c) can embed an editor_t by value and call into the
// synchronous edit primitives directly.
//-------------------------------------------------------------

typedef struct editor_s {
  stringbuf_t *input;     // current user input
  stringbuf_t *extra;     // extra displayed info (for completion menu etc)
  stringbuf_t *hint;      // hint displayed as part of the input
  stringbuf_t *hint_help; // help for a hint.
  ssize_t      pos;       // current cursor position in the input
  ssize_t cur_rows; // current used rows to display our content (including extra
                    // content)
  ssize_t cur_row;  // current row that has the cursor (0 based, relative to the
                    // prompt)
  ssize_t termw;
  bool    modified; // has a modification happened? (used for history navigation
                    // for example)
  bool    disable_undo; // temporarily disable auto undo (for history search)
  ssize_t history_idx;  // current index in the history
  editstate_t *undo;    // undo buffer
  editstate_t *redo;    // redo buffer
  const char  *prompt_text; // text of the prompt before the prompt marker
  // caches
  attrbuf_t *attrs; // reuse attribute buffers
  attrbuf_t *attrs_extra;
} editor_t;

//-------------------------------------------------------------
// Edit primitives shared across the xline internals.
//-------------------------------------------------------------

ic_private bool edit_init(ic_env_t *env, editor_t *eb, const char *prompt_text);
ic_private bool edit_dispatch_key(ic_env_t *env, editor_t *eb, code_t c);
ic_private char *edit_finalize(ic_env_t *env, editor_t *eb, code_t last_c);

//-------------------------------------------------------------
// Editor helpers that cross TU boundaries between edit.c and
// its sibling files (edit_help.c, edit_history.c,
// edit_completion.c).
//-------------------------------------------------------------

ic_private void    edit_refresh(ic_env_t *env, editor_t *eb);
ic_private void    edit_clear(ic_env_t *env, editor_t *eb);
ic_private bool    edit_resize(ic_env_t *env, editor_t *eb);
ic_private void    edit_write_prompt(ic_env_t *env, editor_t *eb, ssize_t row,
                                     bool in_extra);
ic_private ssize_t edit_get_rowcol(ic_env_t *env, editor_t *eb, rowcol_t *rc);
ic_private void    edit_backspace(ic_env_t *env, editor_t *eb);
ic_private void    edit_insert_char(ic_env_t *env, editor_t *eb, char c);
ic_private void edit_insert_unicode(ic_env_t *env, editor_t *eb, unicode_t u);

ic_private void editor_start_modify(editor_t *eb);
ic_private void editor_undo_capture(editor_t *eb);
ic_private void editor_undo_restore(editor_t *eb, bool with_redo);
ic_private void editor_undo_forget(editor_t *eb);

// Entry points implemented in the edit_* sibling files.
ic_private void edit_show_help(ic_env_t *env, editor_t *eb);
ic_private void edit_history_prev(ic_env_t *env, editor_t *eb);
ic_private void edit_history_next(ic_env_t *env, editor_t *eb);
ic_private void edit_history_search_with_current_word(ic_env_t *env,
                                                      editor_t *eb);
ic_private void edit_generate_completions(ic_env_t *env, editor_t *eb,
                                          bool autotab);

//-------------------------------------------------------------
// Cross-TU trace logging (implemented in async.c). Controlled at
// *compile time* by XLINE_TRACE_ENABLED, and at *runtime* by the
// XLINE_TRACE env var (value = log file path). When the compile-time
// switch is off (the default) both calls expand to (void)0 so release
// builds pay zero cost — no function call, no varargs marshalling,
// not even a getenv(). Flip it on via e.g.
//   cmake -DCMAKE_C_FLAGS="-DXLINE_TRACE_ENABLED=1"
// to revive the tracer while debugging above-region / edit geometry.
// Keep this gated, not deleted — the bytes-level dumps (edit_refresh,
// emit_bytes, erase_trailing …) were invaluable for the "thinking ate
// N rows" regression and will almost certainly be needed again.
//-------------------------------------------------------------
#ifndef XLINE_TRACE_ENABLED
#define XLINE_TRACE_ENABLED 0
#endif

#if XLINE_TRACE_ENABLED
ic_private void xline_trace(const char *fmt, ...);
ic_private void xline_trace_bytes(const char *label, const char *data,
                                  ssize_t len);
#else
#define xline_trace(...)              ((void)0)
#define xline_trace_bytes(label, data, len) ((void)0)
#endif

#endif // IC_EDIT_H
