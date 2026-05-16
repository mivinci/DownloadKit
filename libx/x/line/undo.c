/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "color.h"
#include "completions.h"
#include "env.h"
#include "line.h"
#include "platform.h"
#include "str.h"
#include "stringbuf.h"
#include "undo.h"
#include "unicode.h"
#include <x/base/log.h>

//-------------------------------------------------------------
// edit state
//-------------------------------------------------------------
struct editstate_s {
  struct editstate_s *next;
  const char         *input; // input
  ssize_t             pos;   // cursor position
};

ic_private void editstate_init(editstate_t **es) {
  *es = NULL;
}

ic_private void editstate_done(editstate_t **es) {
  while (*es != NULL) {
    editstate_t *next = (*es)->next;
    free((void *)(*es)->input);
    free(*es);
    *es = next;
  }
  *es = NULL;
}

ic_private void editstate_capture(editstate_t **es, const char *input, ssize_t pos) {
  if (input == NULL) input = "";
  // alloc
  editstate_t *entry = (editstate_t *)calloc(1, sizeof(editstate_t));
  if (entry == NULL) return;
  // initialize
  entry->input = ic_strdup(input);
  entry->pos   = pos;
  if (entry->input == NULL) {
    free(entry);
    return;
  }
  // and push
  entry->next = *es;
  *es         = entry;
}

// caller should free *input
ic_private bool editstate_restore(editstate_t **es, const char **input, ssize_t *pos) {
  if (*es == NULL) return false;
  // pop
  editstate_t *entry = *es;
  *es                = entry->next;
  *input             = entry->input;
  *pos               = entry->pos;
  free(entry);
  return true;
}
