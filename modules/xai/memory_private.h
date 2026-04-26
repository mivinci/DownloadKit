/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_private.h - L1/L2 memory pipeline internal structures
 *
 * Defines the data types that flow between the Session-layer hooks
 * (on_produced / on_finalizing) and the Agent-layer memory queue.
 * Not installed; not part of the public API.
 */

#ifndef XAI_MEMORY_PRIVATE_H
#define XAI_MEMORY_PRIVATE_H

#include <stddef.h>
#include <xbase/base.h>
#include <xbase/mpsc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Observation kinds ─────────────────────────────────────────── */

/**
 * @brief Classification of an L1 observation.
 */
XDEF_ENUM(xAiObservationKind) {
  xAiObservationKind_Preference, /**< Explicit user preference
                                       ("I prefer concise code")       */
  xAiObservationKind_Fact,       /**< Factual statement
                                       ("Project uses UTF-8 encoding") */
  xAiObservationKind_Decision,   /**< A decision made
                                       ("Decided to use SQLite")       */
  xAiObservationKind_Summary,    /**< Session-level summary              */
};

/* ── Observation ───────────────────────────────────────────────── */

/**
 * @brief A structured observation extracted from conversation output.
 *
 * Produced by the L1 extraction hooks (on_produced / on_finalizing)
 * and queued via xMpscPush for asynchronous persistence by the
 * Agent's memory timer.
 */
XDEF_STRUCT(xAiObservation) {
  xAiObservationKind kind;       /**< What category this observation is. */
  const char        *content;    /**< Structured summary (not raw text),
                                      owned (strdup'd).                   */
  float              confidence; /**< 0..1; rule-based extraction = 1.0,
                                      LLM-based extraction = model score. */
  const char        *source_id;  /**< Session id that produced this,
                                      owned (strdup'd).                   */
};

/* ── Memory node (MPSC queue element) ──────────────────────────── */

/**
 * @brief MPSC queue node that wraps an xAiObservation.
 *
 * The agent pushes these into its memory_queue via xMpscPush from
 * on_produced and on_finalizing. The memory timer drains them via
 * xMpscPop and writes to disk.
 */
XDEF_STRUCT(xAiMemoryNode) {
  xMpsc           next;     /**< Intrusive MPSC link (must be first). */
  xAiObservation  obs;      /**< The payload.                         */
};

/**
 * @brief Allocate and initialise a memory node.
 *
 * Deep-copies strings from @p obs (content and source_id are
 * strdup'd). The caller owns the returned node and should push it
 * into the agent's memory queue or free it on failure.
 *
 * @param obs        Observation to copy.
 * @return           A new node, or NULL on allocation failure.
 */
XCAPI(xAiMemoryNode *) xAiMemoryNodeCreate(const xAiObservation *obs);

/**
 * @brief Free a memory node and its owned strings.
 *
 * Safe to call on NULL (no-op).
 *
 * @param node  Node to free.
 */
XCAPI(void) xAiMemoryNodeDestroy(xAiMemoryNode *node);

#ifdef __cplusplus
}
#endif

#endif /* XAI_MEMORY_PRIVATE_H */