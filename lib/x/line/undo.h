/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef IC_UNDO_H
#define IC_UNDO_H

#include "platform.h"

//-------------------------------------------------------------
// Edit state
//-------------------------------------------------------------
struct editstate_s;
typedef struct editstate_s editstate_t;

ic_private void editstate_init(editstate_t **es);
ic_private void editstate_done(editstate_t **es);
ic_private void editstate_capture(editstate_t **es,
                                  const char *input, ssize_t pos);
ic_private bool editstate_restore(editstate_t **es,
                                  const char **input,
                                  ssize_t *pos); // caller needs to free input

#endif // IC_UNDO_H
