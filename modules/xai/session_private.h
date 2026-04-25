/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_private.h - Internal layout of xAiSession_ and its
 *                     cross-TU helpers shared with query.c
 *
 * Exposed only to session.c, query.c and their tests. Other xai
 * translation units must go through session.h.
 */

#ifndef XAI_SESSION_PRIVATE_H
#define XAI_SESSION_PRIVATE_H

#include <xai/session.h>

#include "query_private.h"
#include "turn_private.h" /* struct xAiSessionMsg_, ai_session_msg_free */

#include <stddef.h>

/**
 * @brief The session instance.
 *
 * Accessed by session.c and query.c (through the back-pointer on
 * xAiQuery_). Session holds everything durable across a conversation:
 * agent reference, caller-facing callbacks, resolved configuration,
 * and the rolling history. Everything that belongs to "one in-flight
 * run of the tool loop" lives on the embedded Query (@c query) —
 * see query_private.h.
 */
struct xAiSession_ {
  xAiAgent            agent; /* borrowed                              */
  xAiSessionCallbacks cbs;   /* copied by value                       */

  /* ── Configuration snapshot (borrowed from conf or agent) ─────── */
  const char *system_prompt; /* borrowed from conf or agent           */
  const char *model;         /* borrowed from conf or agent           */
  int         max_turns;     /* resolved (>0) or 0 = unlimited        */
  int         max_tokens;    /* resolved per-round cap                */
  size_t      context_budget;

  /* Structured context-budget policy, captured from conf by value
   * at create time. Zero-initialised (Disabled) means "no budget
   * enforcement" and is the backward-compatible default; the
   * enforcement pipeline itself lands in follow-up commits. Not
   * inherited from the agent today — see session.h for rationale. */
  xAiBudgetConf budget;

  /* ── Session-lifetime properties (stamped at create, immutable) ── */
  xAiInputOrigin         origin;           /* default User on zero    */
  xAiSessionFinalizingFn on_finalizing;    /* NULL = no hook          */
  void                  *finalizing_owner; /* passed back verbatim    */

  /* ── Rolling history (session-owned, flat entries) ────────────── */
  struct xAiSessionMsg_ *history;
  size_t                 n_history;
  size_t                 cap_history;

  /* ── In-flight run state ──────────────────────────────────────── */
  /* At most one Query alive at a time today; the Session itself
   * enforces single-flight by rejecting xAiSessionInput while
   * @c query is non-NULL. NULL means idle (no run in flight). The
   * Query is heap-allocated by xAiQueryCreate and released when its
   * terminal on_done has been forwarded. Promotion to a list is a
   * follow-up for when SystemSynthesized queries start coexisting
   * with user-initiated ones — see docs/todo/xai_architecture.md §8. */
  struct xAiQuery_ *query;
};

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Placed in the shared header so query.c can honour it too. */
#define XAI_SESSION_DEFAULT_MAX_TURNS 16

/* ── Cross-TU helpers (session.c implementers, query.c consumers) ── */

/**
 * @brief Append a (role, text) history entry. @p text is duplicated.
 */
xErrno ai_history_append_text(struct xAiSession_ *s, xAiRole role,
                              const char *text, size_t len);

/**
 * @brief Append an Assistant chain-of-thought entry. @p text is duplicated.
 */
xErrno ai_history_append_thinking(struct xAiSession_ *s, const char *text,
                                  size_t len);

/**
 * @brief Append an Assistant tool_use entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_use(struct xAiSession_ *s, const char *id,
                                  const char *name, const char *args);

/**
 * @brief Append a Tool tool_result entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_result(struct xAiSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error);

#endif /* XAI_SESSION_PRIVATE_H */
