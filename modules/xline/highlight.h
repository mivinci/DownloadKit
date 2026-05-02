/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * highlight.h - Syntax highlighting callback helpers
 */
#pragma once
#ifndef IC_HIGHLIGHT_H
#define IC_HIGHLIGHT_H

#include "attr.h"
#include "bbcode.h"
#include "mem.h"
#include "platform.h"
#include "term.h"

/* ── Syntax highlighting ── */

ic_private void    highlight(alloc_t *mem, bbcode_t *bb, const char *s,
                             attrbuf_t *attrs, xLineHighlightFn *highlighter,
                             void *arg);
ic_private void    highlight_match_braces(const char *s, attrbuf_t *attrs,
                                          ssize_t cursor_pos, const char *braces,
                                          attr_t match_attr, attr_t error_attr);
ic_private ssize_t find_matching_brace(const char *s, ssize_t cursor_pos,
                                       const char *braces, bool *is_balanced);

#endif // IC_HIGHLIGHT_H
