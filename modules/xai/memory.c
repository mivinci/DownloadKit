/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory.c - L1/L2 memory pipeline implementation
 *
 * Implements xAiMemoryNode lifecycle (create / destroy) and
 * JSONL serialisation helpers used by the agent's memory timer.
 */

#include "memory_private.h"

#include <stdlib.h>
#include <string.h>

/* ── xAiMemoryNode lifecycle ────────────────────────────────────────── */

XCAPI(xAiMemoryNode *) xAiMemoryNodeCreate(const xAiObservation *obs) {
  if (!obs) return NULL;

  xAiMemoryNode *node = (xAiMemoryNode *)calloc(1, sizeof(*node));
  if (!node) return NULL;

  /* Deep-copy the observation. Strings are strdup'd so the node
   * owns its data independently of the originating session. */
  node->next = (xMpsc){0};
  node->obs.kind = obs->kind;
  node->obs.content = obs->content ? strdup(obs->content) : NULL;
  node->obs.confidence = obs->confidence;
  node->obs.source_id = obs->source_id ? strdup(obs->source_id) : NULL;

  if ((obs->content && !node->obs.content) ||
      (obs->source_id && !node->obs.source_id)) {
    /* strdup failure — clean up and return NULL. */
    free((void *)node->obs.content);
    free((void *)node->obs.source_id);
    free(node);
    return NULL;
  }

  return node;
}

XCAPI(void) xAiMemoryNodeDestroy(xAiMemoryNode *node) {
  if (!node) return;
  free((void *)node->obs.content);
  free((void *)node->obs.source_id);
  free(node);
}