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

#include "budget_private.h" /* xAiBudgetCalibrator                    */
#include "query_private.h"

#include <stddef.h>
#include <xbase/array.h>

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

  /* Structured context-budget policy, captured from conf by value
   * at create time. Zero-initialised (Disabled) means "no budget
   * enforcement" and is the backward-compatible default; the
   * enforcement pipeline itself lands in follow-up commits. Not
   * inherited from the agent today — see session.h for rationale. */
  xAiBudgetConf budget;

  /* Budget-event callback, copied from budget.on_budget_event at
   * create time. Stored separately so the session can fire events
   * without re-reading the budget conf (the callback pointer lives
   * alongside the other session-level callbacks for locality). */
  xAiBudgetEventFunc on_budget_event;
  void              *budget_event_ud;

  /* Online calibration state for the token estimator. Initialised
   * to identity (factor = 1.0) by xAiSessionCreate; updated from
   * sess_fwd_on_done whenever a run's first round returns a usage
   * block with a known prompt_tokens (the gate only runs before the
   * first round, so first_round_prompt_tokens is the only value
   * that cleanly maps to the gate estimate). Consulted by the budget
   * gate on the next xAiSessionInput so a session running against
   * a model whose true token count drifts from the coarse bytes/4
   * heuristic self-corrects within a handful of turns. See
   * ai_budget_calibrator_update() for the exact opt-in rules. */
  xAiBudgetCalibrator budget_calibrator;

  /* The calibrated pre-submit estimate of the last turn that
   * cleared the budget gate, or 0 when no run is in flight /
   * never-calibrated. Paired with first_round_prompt_tokens from
   * sess_fwd_on_done to produce one calibration observation per
   * run (including multi-round tool loops). */
  size_t last_prompt_estimate;

  /* The first-round prompt_tokens from the most recently completed
   * run, or -1 if the provider never reported usage. Exposed via
   * xAiBudgetGateInfo so the caller can compare estimated vs actual
   * without the inflation that later tool-loop rounds introduce. */
  int last_first_round_prompt_tokens;

  /* ── Compact-in-progress state (SummarizeOldest policy) ──────
   *
   * When the budget gate fires on SummarizeOldest, the Session
   * launches an internal summary Query that compresses old history
   * into one System entry. During this compaction:
 *   - compacting == 1 signals "compact query in flight";
 *   - compact_keep_idx records the earliest history index that
 *     survives the compact (entries before it will be replaced by
 *     the summary);
 *   - budget enforcement is implicitly disabled because the
 *     compact Query is driven by session_enforce_budget_ which
 *     gates on s->compacting, preventing recursive budget checks.
   *
   * All three are zero when no compact is in progress.
   */
  int             compacting;       /* 1 = compact query in flight           */
  size_t          compact_keep_idx; /* earliest index to keep after compact  */

  /* ── Session-lifetime properties (stamped at create, immutable) ── */
  xAiInputOrigin           origin;           /* default User on zero    */
  xAiSessionFinalizingFunc on_finalizing;    /* NULL = no hook          */
  void                    *finalizing_owner; /* passed back verbatim    */

  /* L1 memory-preservation callback. Fired when the session is about
   * to discard history entries (TruncateOldest / SummarizeOldest
   * compact), and once at teardown with the full remaining history.
   * Copied from xAiSessionConf at create time. NULL = no L1 hook. */
  xAiSessionL1PreserveFunc on_l1_preserve;
  void                    *l1_preserve_owner;

  /* Session identifier: borrowed from conf at create time, or
   * auto-generated by the agent in xAiAgentCreateSession. May be
   * NULL. Used as part of the L1 memory file path. */
  const char *session_id;

  /* ── Rolling history (session-owned, flat entries) ────────────── */
  xArray history_arr;

  /* ── In-flight run state ──────────────────────────────────────── */
  /* At most one Query alive at a time today; the Session itself
   * enforces single-flight by rejecting xAiSessionInput while
   * @c query is non-NULL. NULL means idle (no run in flight). The
   * Query is heap-allocated by xAiQueryCreate and released when its
   * terminal on_done has been forwarded. Promotion to a list is a
   * follow-up for when SystemSynthesized queries start coexisting
   * with user-initiated ones — see docs/todo/xai_architecture.md §8. */
  struct xAiQuery_ *query;

  /* ── Sidecar query state ──────────────────────────────────────── */
  /* When an async tool call (e.g. shell) blocks the main Query and
   * no streaming output has been received for sidecar_idle_ms, the
   * Session launches a lightweight sidecar Query so the AI can
   * inspect the situation and take action (send stdin, cancel, etc).
   * The sidecar coexists with the main Query — it does NOT replace
   * it and does NOT occupy @c query. At most one sidecar is alive
   * at a time (sidecar != NULL acts as the re-entrancy guard). */

  struct xAiQuery_ *sidecar;             /* NULL = no sidecar active            */
  xEventTimer       sidecar_idle_timer;  /* idle-detection timer handle         */
  uint64_t          sidecar_idle_ms;     /* 0 = disabled (from session conf)    */
  char             *sidecar_tool_use_id; /* async tool that triggered sidecar   */
  xArray            sidecar_output;      /* accumulated tool output chunks      */
  uint64_t          sidecar_last_output_ms; /* monotonic ms of last chunk        */
};

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Placed in the shared header so query.c can honour it too. */
#define XAI_SESSION_DEFAULT_MAX_TURNS 16

/* System prompt used by the internal summary Query when the
 * SummarizeOldest budget policy is active. Instructs the model to
 * produce a concise summary of the conversation segment it receives.
 * The placeholder [N] will be replaced with the number of messages
 * being summarised. */
#define XAI_SUMMARY_SYSTEM_PROMPT                                       \
  "Summarise the following %zu messages concisely in no more than 200 " \
  "words. Preserve all names, numbers, decisions and key facts. "       \
  "Do NOT add any information that was not in the original messages."

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
