/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * undo.h - Undo/redo buffer
 */
#pragma once
#ifndef IC_UNDO_H
#define IC_UNDO_H

#include "mem.h"
#include "platform.h"

/* ── Edit state ── */
struct editstate_s;
typedef struct editstate_s editstate_t;

ic_private void editstate_init(editstate_t **es);
ic_private void editstate_done(alloc_t *mem, editstate_t **es);
ic_private void editstate_capture(alloc_t *mem, editstate_t **es,
                                  const char *input, ssize_t pos);
ic_private bool editstate_restore(alloc_t *mem, editstate_t **es,
                                  const char **input,
                                  ssize_t *pos); // caller needs to free input

#endif // IC_UNDO_H
