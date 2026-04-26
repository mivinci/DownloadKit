/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent_private.h - Concrete layout of xAiAgent for intra-module use
 *
 * The xai module keeps the agent representation intentionally flat:
 * everything in xAiAgentConf is copied by value into struct xAiAgent_,
 * and session.c reaches in directly for provider / tools / limits.
 * Not installed; not part of the public API.
 */

#ifndef XAI_AGENT_PRIVATE_H
#define XAI_AGENT_PRIVATE_H

#include <stddef.h>
#include <xai/agent.h>
#include <xai/provider.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/event.h>
#include <xbase/mpsc.h>
#include <xbase/task.h>

struct xAiSession_;  /* forward — full definition in session_private.h */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Concrete agent layout.
 *
 * All fields are captured from xAiAgentConf at Create time and are
 * then read-only for the agent's lifetime. Strings and the tools
 * array are borrowed from the caller, as documented in agent.h.
 */
struct xAiAgent_ {
  xEventLoop         loop;           /**< Event loop (never NULL).       */
  xAiProvider        provider;       /**< Backing provider (never NULL). */

  const char        *model;          /**< Default model id, borrowed.    */
  const char        *system_prompt;  /**< Default system prompt.         */

  const xAiTool    **tools;          /**< Borrowed tool array.           */
  size_t             n_tools;

  xTaskGroup         task_group;     /**< Optional worker pool.          */

  int                max_turns;      /**< 0 = library default.           */
  int                max_tokens;     /**< 0 = provider default.          */

  /* ── L2 Memory persistence ──────────────────────────────────────────
   *
   * When memory_dir is non-NULL the agent maintains an MPSC queue
   * (memory_head / memory_tail) for L1-extracted observations and
   * a periodic timer (memory_timer) that drains the queue and writes
   * each observation as one JSONL line to a file under memory_dir.
   *
   * File layout:
   *   <memory_dir>/<agent_id>/observations.jsonl
   *
   * The timer fires at a configurable interval (default 30 seconds)
   * and is created by xAiAgentStart(). It is cancelled and drained
   * in xAiAgentDestroy().
   */
  const char        *memory_dir;     /**< Borrowed from conf, may be NULL. */
  xMpsc             *memory_head;    /**< MPSC queue head (or NULL).       */
  xMpsc             *memory_tail;    /**< MPSC queue tail (or NULL).       */
  xEventTimer        memory_timer;   /**< Periodic drain timer (or NULL).  */

  struct xAiSession_ *default_session; /**< Built-in alignment session,
                                            or NULL if the agent was
                                            created without one. Owned. */
};

#ifdef __cplusplus
}
#endif

#endif /* XAI_AGENT_PRIVATE_H */
