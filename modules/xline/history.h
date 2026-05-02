/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * history.h - History storage and retrieval
 */
#pragma once
#ifndef IC_HISTORY_H
#define IC_HISTORY_H

#include "mem.h"
#include "platform.h"

/* ── History ── */

struct history_s;
typedef struct history_s history_t;

ic_private history_t *history_new(alloc_t *mem);
ic_private void       history_free(history_t *h);
ic_private void       history_clear(history_t *h);
ic_private bool       history_enable_duplicates(history_t *h, bool enable);
ic_private ssize_t    history_count(const history_t *h);

ic_private void history_load_from(history_t *h, const char *fname,
                                  long max_entries);
ic_private void history_load(history_t *h);
ic_private void history_save(const history_t *h);

ic_private bool        history_push(history_t *h, const char *entry);
ic_private bool        history_update(history_t *h, const char *entry);
ic_private const char *history_get(const history_t *h, ssize_t n);
ic_private void        history_remove_last(history_t *h);

ic_private bool history_search(const history_t *h, ssize_t from,
                               const char *search, bool backward, ssize_t *hidx,
                               ssize_t *hpos);

#endif // IC_HISTORY_H
