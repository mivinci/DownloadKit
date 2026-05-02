/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_EDITLINE_H
#define IC_EDITLINE_H

#include "attr.h"
#include "env.h"
#include "mem.h"
#include "platform.h"
#include "stringbuf.h"
#include "undo.h"

//-------------------------------------------------------------
// The editor state
//
// Lifted from editline.c so that other internal translation units
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
  alloc_t     *mem;         // allocator
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
// Editor helpers that cross TU boundaries between editline.c and
// its sibling files (editline_help.c, editline_history.c,
// editline_completion.c).
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

// Entry points implemented in the editline_* sibling files.
ic_private void edit_show_help(ic_env_t *env, editor_t *eb);
ic_private void edit_history_prev(ic_env_t *env, editor_t *eb);
ic_private void edit_history_next(ic_env_t *env, editor_t *eb);
ic_private void edit_history_search_with_current_word(ic_env_t *env,
                                                      editor_t *eb);
ic_private void edit_generate_completions(ic_env_t *env, editor_t *eb,
                                          bool autotab);

#endif // IC_EDITLINE_H
