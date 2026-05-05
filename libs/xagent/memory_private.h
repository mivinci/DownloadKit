/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_private.h - Shared layout for xAgentMemory implementations
 *
 * Every backend embeds struct xAgentMemory_ as its first field so
 * the generic wrappers in memory.c can upcast a backend-specific
 * struct into the vtable dispatcher. Not installed; consumed only
 * by memory.c and the in-tree backends (memory_jsonl.c, tests).
 */

#ifndef XAGENT_MEMORY_PRIVATE_H
#define XAGENT_MEMORY_PRIVATE_H

#include <xagent/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Base layout for every memory backend.
 *
 * Backends extend this struct by placing their own state after the
 * vt pointer:
 *
 *   struct my_backend_ {
 *     struct xAgentMemory_ base; // MUST be first
 *     // ...backend-specific fields...
 *   };
 *
 * The generic xAgentMemory* wrappers only touch base.vt; everything
 * else is backend-private.
 */
struct xAgentMemory_ {
  const xAgentMemoryVTable *vt; /* never NULL after construction */
};

#ifdef __cplusplus
}
#endif

#endif /* XAGENT_MEMORY_PRIVATE_H */
