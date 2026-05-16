/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

//-------------------------------------------------------------
// Completion / hint menu rendering for xline.
//-------------------------------------------------------------

#include "bbcode.h"
#include "completions.h"
#include "edit.h"
#include "env.h"
#include "platform.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"
#include <x/base/log.h>

// return true if anything changed
static bool edit_complete(ic_env_t *env, editor_t *eb, ssize_t idx) {
  editor_start_modify(eb);
  ssize_t newpos = completions_apply(env->completions, idx, eb->input, eb->pos);
  if (newpos < 0) {
    editor_undo_restore(eb, false);
    return false;
  }
  eb->pos = newpos;
  edit_refresh(env, eb);
  return true;
}

static bool edit_complete_longest_prefix(ic_env_t *env, editor_t *eb) {
  editor_start_modify(eb);
  ssize_t newpos = completions_apply_longest_prefix(env->completions, eb->input, eb->pos);
  if (newpos < 0) {
    editor_undo_restore(eb, false);
    return false;
  }
  eb->pos = newpos;
  edit_refresh(env, eb);
  return true;
}

static void sbuf_append_tagged(stringbuf_t *sb, const char *tag, const char *content) {
  sbuf_appendf(sb, "[%s]", tag);
  sbuf_append(sb, content);
  sbuf_append(sb, "[/]");
}

static void editor_append_completion(ic_env_t *env, editor_t *eb, ssize_t idx, ssize_t width,
                                     bool numbered, bool selected) {
  const char *help    = NULL;
  const char *display = completions_get_display(env->completions, idx, &help);
  if (display == NULL) return;
  if (numbered) {
    sbuf_appendf(eb->extra, "[ic-info]%s%2zd [/]",
                 (selected ? (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : "*") : " "), 1 + idx);
    width -= 4;
  }

  if (width > 0) {
    sbuf_appendf(eb->extra, "[width=\"%zd;left; ;on\"]", width);
  }
  if (selected) {
    sbuf_append(eb->extra, "[ic-emphasis]");
  }
  sbuf_append(eb->extra, display);
  if (selected) {
    sbuf_append(eb->extra, "[/ic-emphasis]");
  }
  if (help != NULL) {
    sbuf_append(eb->extra, "  ");
    sbuf_append_tagged(eb->extra, "ic-info", help);
  }
  if (width > 0) {
    sbuf_append(eb->extra, "[/width]");
  }
}

// 2 and 3 column output up to 80 wide
#define IC_DISPLAY2_MAX   34
#define IC_DISPLAY2_COL   (3 + IC_DISPLAY2_MAX)
#define IC_DISPLAY2_WIDTH (2 * IC_DISPLAY2_COL + 2) // 75

#define IC_DISPLAY3_MAX   21
#define IC_DISPLAY3_COL   (3 + IC_DISPLAY3_MAX)
#define IC_DISPLAY3_WIDTH (3 * IC_DISPLAY3_COL + 2 * 2) // 76

static void editor_append_completion2(ic_env_t *env, editor_t *eb, ssize_t col_width, ssize_t idx1,
                                      ssize_t idx2, ssize_t selected) {
  editor_append_completion(env, eb, idx1, col_width, true, (idx1 == selected));
  sbuf_append(eb->extra, "  ");
  editor_append_completion(env, eb, idx2, col_width, true, (idx2 == selected));
}

static void editor_append_completion3(ic_env_t *env, editor_t *eb, ssize_t col_width, ssize_t idx1,
                                      ssize_t idx2, ssize_t idx3, ssize_t selected) {
  editor_append_completion(env, eb, idx1, col_width, true, (idx1 == selected));
  sbuf_append(eb->extra, "  ");
  editor_append_completion(env, eb, idx2, col_width, true, (idx2 == selected));
  sbuf_append(eb->extra, "  ");
  editor_append_completion(env, eb, idx3, col_width, true, (idx3 == selected));
}

static ssize_t edit_completions_max_width(ic_env_t *env, ssize_t count) {
  ssize_t max_width = 0;
  for (ssize_t i = 0; i < count; i++) {
    const char *help = NULL;
    ssize_t     w =
      bbcode_column_width(env->bbcode, completions_get_display(env->completions, i, &help));
    if (help != NULL) {
      w += 2 + bbcode_column_width(env->bbcode, help);
    }
    if (w > max_width) {
      max_width = w;
    }
  }
  return max_width;
}

static void edit_completion_menu(ic_env_t *env, editor_t *eb, bool more_available) {
  ssize_t count           = completions_count(env->completions);
  ssize_t count_displayed = count;
  assert(count > 1);
  ssize_t selected      = (env->complete_nopreview ? 0 : -1); // select first or none
  ssize_t percolumn     = count;
  ssize_t scroll_offset = 0; // first completion index visible in the grid

  // While rendering a menu frame we own eb->extra: suspend the host's
  // refresh_prepare hook (used by xline below-panel) so it doesn't
  // overwrite the menu contents. We restore it right after reading the
  // key, so subsequent refreshes (menu-exit, esc, page-down, ...) rebuild
  // the below panel as expected.
  void (*saved_refresh_prepare)(void *, void *) = env->refresh_prepare;
  void *saved_refresh_prepare_arg               = env->refresh_prepare_arg;

again:
  env->refresh_prepare     = NULL;
  env->refresh_prepare_arg = NULL;
  // show first 9 (or 8) completions
  sbuf_clear(eb->extra);
  ssize_t twidth = term_get_width(env->term) - 1;
  ssize_t colwidth;
  if (count > 3 && ((colwidth = 4 + edit_completions_max_width(env, count < 9 ? count : 9)) * 3 +
                    2 * 2) < twidth) {
    // display as a 3 column block
    ssize_t grid_size = (count > 9 ? 9 : count);
    // clamp scroll_offset so the grid always fills up to grid_size items
    if (scroll_offset + grid_size > count) scroll_offset = count - grid_size;
    if (scroll_offset < 0) scroll_offset = 0;
    count_displayed = grid_size;
    percolumn       = 3;
    for (ssize_t rw = 0; rw < percolumn; rw++) {
      if (rw > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion3(env, eb, colwidth, scroll_offset + rw,
                                scroll_offset + percolumn + rw,
                                scroll_offset + (2 * percolumn) + rw, selected);
    }
  } else if (count > 4 &&
             ((colwidth = 4 + edit_completions_max_width(env, count < 8 ? count : 8)) * 2 + 2) <
               twidth) {
    // display as a 2 column block if some entries are too wide for three
    // columns
    ssize_t grid_size = (count > 8 ? 8 : count);
    if (scroll_offset + grid_size > count) scroll_offset = count - grid_size;
    if (scroll_offset < 0) scroll_offset = 0;
    count_displayed = grid_size;
    percolumn       = (count_displayed <= 6 ? 3 : 4);
    for (ssize_t rw = 0; rw < percolumn; rw++) {
      if (rw > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion2(env, eb, colwidth, scroll_offset + rw,
                                scroll_offset + percolumn + rw, selected);
    }
  } else {
    // display as a list
    ssize_t grid_size = (count > 9 ? 9 : count);
    if (scroll_offset + grid_size > count) scroll_offset = count - grid_size;
    if (scroll_offset < 0) scroll_offset = 0;
    count_displayed = grid_size;
    percolumn       = count_displayed;
    for (ssize_t i = 0; i < count_displayed; i++) {
      if (i > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion(env, eb, scroll_offset + i, -1, true /* numbered */,
                               selected == (scroll_offset + i));
    }
  }
  if (scroll_offset + count_displayed < count) {
    // Note: ctrl-j (KEY_LINEFEED) is accepted here as a fallback on dumb
    // ttys where page-down doesn't decode (see the KEY_PAGEDOWN handler
    // below), but we deliberately don't advertise it: ctrl-j == LF and
    // users reasonably expect it to behave like Enter everywhere, so
    // putting it in the hint text would be misleading.
    if (more_available) {
      sbuf_append(eb->extra, "\n[ic-info](press page-down to see "
                             "all further completions)[/]");
    } else {
      sbuf_appendf(eb->extra,
                   "\n[ic-info](press page-down to see all %zd "
                   "completions)[/]",
                   count);
    }
  }
  if (!env->complete_nopreview && selected >= 0 && selected >= scroll_offset &&
      selected < scroll_offset + count_displayed) {
    edit_complete(env, eb, selected);
    editor_undo_restore(eb, false);
  } else {
    edit_refresh(env, eb);
  }

  // read here; if not a valid key, push it back and return to main event loop
  code_t c = tty_read(env->tty);
  if (tty_term_resize_event(env->tty)) {
    edit_resize(env, eb);
  }
  // Menu frame is done: restore the host's refresh_prepare hook so that
  // every edit_refresh() triggered from here on (menu-exit, esc, etc.)
  // rebuilds the below panel.
  env->refresh_prepare     = saved_refresh_prepare;
  env->refresh_prepare_arg = saved_refresh_prepare_arg;
  sbuf_clear(eb->extra);

  // direct selection? (numbers refer to visible items 1-9)
  if (c >= '1' && c <= '9') {
    ssize_t i = scroll_offset + (c - '1');
    if (i < count) {
      selected = i;
      c        = KEY_ENTER;
    }
  }

  // Grid navigation helpers. Items are laid out column-major:
  //   index = col * percolumn + row
  // so percolumn == rows per column; columns are ceil(count_displayed/percolumn).
  // cur_row / cur_col are positions *within the visible grid* (relative to scroll_offset).
  ssize_t ncols = (count_displayed + percolumn - 1) / percolumn;
  if (ncols < 1) ncols = 1;
  ssize_t rel     = (selected < 0 ? 0 : selected - scroll_offset);
  ssize_t cur_row = (rel < 0 ? 0 : rel % percolumn);
  ssize_t cur_col = (rel < 0 ? 0 : rel / percolumn);

  // process commands
  if (c == KEY_TAB) {
    // Tab cycles through all entries in display order (row-major),
    // matches typical shell behaviour.
    if (selected < 0) {
      selected = scroll_offset;
    } else {
      selected++;
      if (selected >= count) selected = 0;
      // ensure the selected item is visible
      if (selected < scroll_offset || selected >= scroll_offset + count_displayed) {
        scroll_offset = selected - (selected % percolumn);
        if (scroll_offset + count_displayed > count) scroll_offset = count - count_displayed;
        if (scroll_offset < 0) scroll_offset = 0;
      }
    }
    goto again;
  } else if (c == KEY_SHIFT_TAB) {
    if (selected < 0) {
      selected = scroll_offset + count_displayed - 1;
    } else {
      selected--;
      if (selected < 0) selected = count - 1;
      if (selected < scroll_offset || selected >= scroll_offset + count_displayed) {
        scroll_offset = selected - (selected % percolumn);
        if (scroll_offset + count_displayed > count) scroll_offset = count - count_displayed;
        if (scroll_offset < 0) scroll_offset = 0;
      }
    }
    goto again;
  } else if (c == KEY_DOWN) {
    // move down within the current column; scroll the view if at the bottom
    if (selected < 0) {
      selected = scroll_offset;
    } else {
      ssize_t nr = cur_row + 1;
      if (nr >= percolumn || cur_col * percolumn + nr >= count_displayed) {
        // at the bottom of the visible grid
        if (scroll_offset + count_displayed < count) {
          // scroll down one row and keep the cursor at the bottom
          scroll_offset++;
          selected = scroll_offset + cur_col * percolumn + cur_row;
        } else {
          // wrap to the top: go to the first item in this column
          if (scroll_offset > 0) {
            scroll_offset = 0;
          }
          selected = scroll_offset + cur_col * percolumn;
        }
      } else {
        selected = scroll_offset + cur_col * percolumn + nr;
      }
    }
    goto again;
  } else if (c == KEY_UP) {
    if (selected < 0) {
      selected = scroll_offset;
    } else {
      ssize_t nr = cur_row - 1;
      if (nr < 0) {
        // at the top of the visible grid
        if (scroll_offset > 0) {
          // scroll up one row and keep the cursor at the top
          scroll_offset--;
          selected = scroll_offset + cur_col * percolumn + 0;
        } else {
          // wrap to the bottom: go to the last item in this column
          ssize_t last_in_col = count - cur_col * percolumn - 1;
          if (last_in_col > percolumn - 1) last_in_col = percolumn - 1;
          nr = last_in_col;
          // adjust scroll_offset so the last items are visible
          scroll_offset = (count - count_displayed);
          if (scroll_offset < 0) scroll_offset = 0;
          selected = scroll_offset + cur_col * percolumn + nr;
        }
      } else {
        selected = scroll_offset + cur_col * percolumn + nr;
      }
    }
    goto again;
  } else if (c == KEY_RIGHT && ncols > 1) {
    // move to the same row in the next column; wrap around
    if (selected < 0) {
      selected = scroll_offset;
    } else {
      ssize_t nc = cur_col + 1;
      if (nc >= ncols || scroll_offset + nc * percolumn + cur_row >= count) {
        nc = 0;
      }
      selected = scroll_offset + nc * percolumn + cur_row;
    }
    goto again;
  } else if (c == KEY_LEFT && ncols > 1) {
    if (selected < 0) {
      selected = scroll_offset;
    } else {
      ssize_t nc = cur_col - 1;
      if (nc < 0) {
        // wrap to the rightmost column that has this row filled
        nc = ncols - 1;
        while (nc > 0 && scroll_offset + nc * percolumn + cur_row >= count)
          nc--;
      }
      selected = scroll_offset + nc * percolumn + cur_row;
    }
    goto again;
  } else if (c == KEY_F1) {
    edit_show_help(env, eb);
    goto again;
  } else if (c == KEY_ESC) {
    completions_clear(env->completions);
    edit_refresh(env, eb);
    c = 0; // ignore and return
  } else if (selected >= 0 && (c == KEY_ENTER || c == KEY_END)) {
    // select the current entry
    assert(selected < count);
    c = 0;
    edit_complete(env, eb, selected);
    if (env->complete_autotab) {
      tty_code_pushback(env->tty,
                        KEY_EVENT_AUTOTAB); // immediately try to complete again
    }
  } else if (!env->complete_nopreview && selected >= 0 && !code_is_virt_key(c)) {
    // if in preview mode and the user has highlighted an entry, accept it
    // and exit the menu; the typed key then flows through to the main
    // dispatcher (via the pushback below) so it lands after the accepted
    // completion text.
    assert(selected < count);
    edit_complete(env, eb, selected);
  } else if (!code_is_virt_key(c)) {
    // No entry was highlighted and the user typed a printable key: treat
    // the key as "menu off, keep typing". Without this branch the menu
    // falls through to the final else (which only refreshes the input
    // region) — leaving the menu's extra rows as a visual residue on
    // screen until the next paint happens to re-cover them. Explicitly
    // refreshing here with a cleared extra guarantees the menu rows are
    // erased in the same frame the key is pushed back.
    completions_clear(env->completions);
    edit_refresh(env, eb);
  } else if ((c == KEY_PAGEDOWN || c == KEY_LINEFEED) && count > 9) {
    // show all completions
    c = 0;
    if (more_available) {
      // generate all entries (up to the max (= 1000))
      count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos,
                                   IC_MAX_COMPLETIONS_TO_SHOW);
    }
    rowcol_t rc;
    edit_get_rowcol(env, eb, &rc);
    edit_clear(env, eb);
    edit_write_prompt(env, eb, 0, false);
    term_writeln(env->term, "");
    for (ssize_t i = 0; i < count; i++) {
      const char *display = completions_get_display(env->completions, i, NULL);
      if (display != NULL) {
        bbcode_println(env->bbcode, display);
      }
    }
    if (count >= IC_MAX_COMPLETIONS_TO_SHOW) {
      bbcode_println(env->bbcode, "[ic-info]... and more.[/]");
    } else {
      bbcode_printf(env->bbcode, "[ic-info](%zd possible completions)[/]\n", count);
    }
    for (ssize_t i = 0; i < rc.row + 1; i++) {
      term_write(env->term, " \n");
    }
    eb->cur_rows = 0;
    edit_refresh(env, eb);
  } else {
    edit_refresh(env, eb);
  }
  // done
  completions_clear(env->completions);
  if (c != 0) tty_code_pushback(env->tty, c);
}

ic_private void edit_generate_completions(ic_env_t *env, editor_t *eb, bool autotab) {
  XDEBUG("edit: complete: %zd: %s\n", eb->pos, sbuf_string(eb->input));
  if (eb->pos < 0) return;
  ssize_t count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos,
                                       IC_MAX_COMPLETIONS_TO_TRY);
  bool    more_available = (count >= IC_MAX_COMPLETIONS_TO_TRY);
  if (count <= 0) {
    // no completions
    if (!autotab) {
      term_beep(env->term);
    }
  } else if (count == 1) {
    ssize_t old_pos = eb->pos;
    // complete if only one match
    if (edit_complete(env, eb, 0 /*idx*/) && env->complete_autotab && eb->pos > old_pos) {
      tty_code_pushback(env->tty, KEY_EVENT_AUTOTAB);
    }
  } else {
    // term_beep(env->term);
    if (!more_available) {
      edit_complete_longest_prefix(env, eb);
    }
    completions_sort(env->completions);
    edit_completion_menu(env, eb, more_available);
  }
}
