/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * model.c - ModelRegistry: a tiny id -> (provider, model) table
 *
 * Backed by a plain dynamic array. We expect registries to hold
 * a handful of entries (one per configured LLM in the host app),
 * so a linear scan on lookup is fine and keeps the code auditable.
 * No hashing, no locking — every mutation / lookup must happen on
 * the event-loop thread that owns the agent using the registry.
 */

#include <x/agent/model.h>

#include <stdlib.h>
#include <string.h>

/* Opaque-handle alias: xAgentModelRegistry is a typedef for
 * struct xAgentModelRegistry_ * via XDEF_HANDLE. */
struct xAgentModelRegistry_ {
  xAgentModelSpec *entries; /* entries[0..count-1] are live                */
  size_t           count;
  size_t           cap;
};

/* Free a spec's owned strings (id, model). provider is borrowed. */
static void spec_reset_(xAgentModelSpec *s) {
  if (!s) return;
  free((char *)s->id);
  free((char *)s->model);
  s->id       = NULL;
  s->model    = NULL;
  s->provider = NULL;
}

xAgentModelRegistry xAgentModelRegistryCreate(void) {
  struct xAgentModelRegistry_ *r =
    (struct xAgentModelRegistry_ *)calloc(1, sizeof(*r));
  if (!r) return NULL;
  /* entries/count/cap left zero — first Add grows the buffer. */
  return (xAgentModelRegistry)r;
}

void xAgentModelRegistryDestroy(xAgentModelRegistry reg) {
  if (!reg) return;
  struct xAgentModelRegistry_ *r = (struct xAgentModelRegistry_ *)reg;
  for (size_t i = 0; i < r->count; i++) spec_reset_(&r->entries[i]);
  free(r->entries);
  free(r);
}

xErrno xAgentModelRegistryAdd(xAgentModelRegistry    reg,
                              const xAgentModelSpec *spec) {
  if (!reg || !spec || !spec->id || !*spec->id || !spec->provider)
    return xErrno_InvalidArg;

  struct xAgentModelRegistry_ *r = (struct xAgentModelRegistry_ *)reg;

  /* Duplicate-id rejection: scan the array once. */
  for (size_t i = 0; i < r->count; i++) {
    if (r->entries[i].id && strcmp(r->entries[i].id, spec->id) == 0)
      return xErrno_AlreadyExists;
  }

  /* Grow if needed (geometric: 4, 8, 16, ...). */
  if (r->count == r->cap) {
    size_t new_cap = r->cap ? r->cap * 2 : 4;
    xAgentModelSpec *grown = (xAgentModelSpec *)realloc(
      r->entries, new_cap * sizeof(*grown));
    if (!grown) return xErrno_NoMemory;
    r->entries = grown;
    r->cap     = new_cap;
  }

  /* Duplicate strings up front so a failed strdup leaves the
   * registry untouched. */
  char *id_copy    = strdup(spec->id);
  char *model_copy = spec->model ? strdup(spec->model) : NULL;
  if (!id_copy || (spec->model && !model_copy)) {
    free(id_copy);
    free(model_copy);
    return xErrno_NoMemory;
  }

  xAgentModelSpec *slot = &r->entries[r->count];
  slot->id          = id_copy;
  slot->provider    = spec->provider;
  slot->model       = model_copy;
  slot->temperature = spec->temperature;
  slot->max_tokens  = spec->max_tokens;
  r->count++;
  return xErrno_Ok;
}

const xAgentModelSpec *xAgentModelRegistryGet(xAgentModelRegistry reg,
                                              const char         *id) {
  if (!reg || !id) return NULL;
  struct xAgentModelRegistry_ *r = (struct xAgentModelRegistry_ *)reg;
  for (size_t i = 0; i < r->count; i++) {
    if (r->entries[i].id && strcmp(r->entries[i].id, id) == 0)
      return &r->entries[i];
  }
  return NULL;
}

size_t xAgentModelRegistryCount(xAgentModelRegistry reg) {
  if (!reg) return 0;
  return ((struct xAgentModelRegistry_ *)reg)->count;
}

const xAgentModelSpec *xAgentModelRegistryAt(xAgentModelRegistry reg,
                                             size_t              idx) {
  if (!reg) return NULL;
  struct xAgentModelRegistry_ *r = (struct xAgentModelRegistry_ *)reg;
  if (idx >= r->count) return NULL;
  return &r->entries[idx];
}
