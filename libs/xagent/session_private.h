/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_private.h - Internal layout of xAgentSession_ and its
 *                     cross-TU helpers shared with query.c
 *
 * Exposed only to session.c, query.c and their tests. Other xai
 * translation units must go through session.h.
 */

#ifndef XAGENT_SESSION_PRIVATE_H
#define XAGENT_SESSION_PRIVATE_H

#include <xagent/session.h>

#include "budget_private.h" /* xAgentBudgetCalibrator                    */
#include "query_private.h"

#include <stddef.h>
#include <xbase/array.h>

/**
 * @brief The session instance.
 *
 * Accessed by session.c and query.c (through the back-pointer on
 * xAgentQuery_). Session holds everything durable across a conversation:
 * agent reference, caller-facing callbacks, resolved configuration,
 * and the rolling history. Everything that belongs to "one in-flight
 * run of the tool loop" lives on the embedded Query (@c query) —
 * see query_private.h.
 */
struct xAgentSession_ {
  xAgent                 agent; /* borrowed                              */
  xAgentSessionCallbacks cbs;   /* copied by value                       */

  /* ── Configuration snapshot (borrowed from conf or agent) ─────── */
  const char *system_prompt; /* borrowed from conf or agent           */
  const char *model;         /* borrowed from conf or agent           */
  int         max_turns;     /* resolved (>0) or 0 = unlimited        */
  int         max_tokens;    /* resolved per-round cap                */

  /* Provider override: when non-NULL, used in place of agent->provider
   * the NEXT time the session launches a Query (main or sidecar).
   * NULL = fall back to the agent's provider (the default). Set via
   * xAgentSessionSetProvider(); has no effect on any in-flight Query.
   * Borrowed from the caller; must outlive the session or be cleared
   * (set to NULL) before being destroyed. Not used for internal
   * summary/compact Queries — those always use agent->provider. */
  xAgentProvider provider_override;

  /* Model-name override paired with @ref provider_override when the
   * host switches models via xAgentSessionSetModel(). Owned by the
   * session: set with strdup, freed on replacement and on destroy.
   * NULL means "use @ref model" (no override). Unlike
   * provider_override we must own this string because it typically
   * comes from an xAgentModelSpec whose lifetime is tied to the
   * registry, not to this session. */
  char *model_override;

  /* Structured context-budget policy, captured from conf by value
   * at create time. Zero-initialised (Disabled) means "no budget
   * enforcement" and is the backward-compatible default; the
   * enforcement pipeline itself lands in follow-up commits. Not
   * inherited from the agent today — see session.h for rationale. */
  xAgentBudgetConf budget;

  /* Budget-event callback, copied from budget.on_budget_event at
   * create time. Stored separately so the session can fire events
   * without re-reading the budget conf (the callback pointer lives
   * alongside the other session-level callbacks for locality). */
  xAgentBudgetEventFunc on_budget_event;
  void                 *budget_event_ud;

  /* ── Incremental token bookkeeping ───────────────────────────────
   *
   * Instead of estimating the full history token count every turn
   * and correcting with an EWMA calibrator, we use provider-reported
   * prompt_tokens as the known baseline and only estimate the delta
   * (new entries added since the last provider report).
   *
   *   - known_prompt_tokens: the provider-reported prompt_tokens
   *     from the most recent completed run's first round, or -1 if
   *     no run has completed yet (cold start). This is the precise
   *     token count of everything the provider saw when it processed
   *     the first round — system prompt + memory hits + rolling
   *     history + the user message.
   *
   *   - delta_entries: the number of history entries that have been
   *     appended SINCE the last provider report. Used with the
   *     coarse estimator to compute the delta token count:
   *
   *       current ≈ known_prompt_tokens + estimate(last delta_entries)
   *
   *     This is much more accurate than re-estimating the entire
   *     history because the delta is small (typically 1-5 entries)
   *     and the per-entry error is tiny in aggregate.
   *
   * After a truncate/compact, known_prompt_tokens is invalidated
   * (the history changed in a way we can't precisely quantify) and
   * reset to -1. The next provider report will re-establish it.
   *
   * After a successful GatePassed, we record what the gate computed
   * as the pre-submit estimate in last_gate_total so the on_done
   * handler can compare it with the first-round provider report. */
  int    known_prompt_tokens;      /* -1 = unknown / cold start          */
  size_t delta_entries;            /* entries added since last report     */
  size_t last_gate_total;          /* gate's pre-submit total (or 0)     */

  /* The provider-reported prompt_tokens from the first round of
   * the most recently completed run, or -1 if unavailable.
   * Exposed via xAgentBudgetGateInfo so callers can compare
   * estimated vs actual. */
  int last_first_round_prompt_tokens;

  /* ── Compact-in-progress state (SummarizeOldest policy) ──────
   *
   * When the budget gate fires on SummarizeOldest, the Session
   * launches an internal summary Query that compresses old history
   * into one System entry. During this compaction:
   *   - compacting == 1 signals "compact query in flight";
   *   - compact_head_idx records the end of the HEAD prefix that is
   *     preserved verbatim (history[0..compact_head_idx) survives);
   *   - compact_keep_idx records the start of the RECENT tail that
   *     is preserved verbatim (history[compact_keep_idx..n) survives);
   *   - the MIDDLE segment [compact_head_idx..compact_keep_idx) will
   *     be replaced by one System summary entry on success, and left
   *     untouched on failure;
   *   - budget enforcement is implicitly disabled because the
   *     compact Query is driven by session_enforce_budget_ which
   *     gates on s->compacting, preventing recursive budget checks.
   *
   * All are zero when no compact is in progress.
   */
  int    compacting;       /* 1 = compact query in flight              */
  size_t compact_head_idx; /* end of preserved HEAD prefix (exclusive) */
  size_t compact_keep_idx; /* start of preserved RECENT tail           */

  /* ── Session-lifetime properties (stamped at create, immutable) ── */
  xAgentInputOrigin           origin;           /* default User on zero    */
  xAgentSessionFinalizingFunc on_finalizing;    /* NULL = no hook          */
  void                       *finalizing_owner; /* passed back verbatim    */

  /* L1 memory-preservation callback. Fired when the session is about
   * to discard history entries (TruncateTail / SummarizeOldest
   * compact), and once at teardown with the full remaining history.
   * Copied from xAgentSessionConf at create time. NULL = no L1 hook. */
  xAgentSessionL1PreserveFunc on_l1_preserve;
  void                       *l1_preserve_owner;

  /* Session identifier: borrowed from conf at create time, or
   * auto-generated by the agent in xAgentCreateSession. May be
   * NULL. Used as part of the L1 memory file path. */
  const char *session_id;

  /* Number of leading history_arr entries that have already been
   * persisted by an external xAgentMemory store (typically because
   * the session was primed from that store at create time via
   * xAgentCreateSession). L1 preserve callbacks deliberately skip
   * this prefix so they don't re-append rows the store already
   * owns, avoiding on-disk duplication when a session resumes
   * from prior memory. Increased by the agent's prime path;
   * decreased as Truncated/Compacted events evict primed entries
   * from the front; reset to zero after a Compacted event (the
   * new summary entry is fresh and not yet persisted). Zero for
   * sessions that were never primed, which is the fast path every
   * existing caller hits. */
  size_t persisted_prefix;

  /* ── Rolling history (session-owned, flat entries) ────────────── */
  xArray history_arr;

  /* ── In-flight run state ──────────────────────────────────────── */
  /* At most one Query alive at a time today; the Session itself
   * enforces single-flight by rejecting xAgentSessionInput while
   * @c query is non-NULL. NULL means idle (no run in flight). The
   * Query is heap-allocated by xAgentQueryCreate and released when its
   * terminal on_done has been forwarded. Promotion to a list is a
   * follow-up for when SystemSynthesized queries start coexisting
   * with user-initiated ones — see docs/todo/xai_architecture.md §8. */
  struct xAgentQuery_ *query;

  /* ── Sidecar query state ──────────────────────────────────────── */
  /* When an async tool call (e.g. shell) blocks the main Query and
   * no streaming output has been received for sidecar_idle_ms, the
   * Session launches a lightweight sidecar Query so the AI can
   * inspect the situation and take action (send stdin, cancel, etc).
   * The sidecar coexists with the main Query — it does NOT replace
   * it and does NOT occupy @c query. At most one sidecar is alive
   * at a time (sidecar != NULL acts as the re-entrancy guard). */

  struct xAgentQuery_ *sidecar;       /* NULL = no sidecar active            */
  xEventTimer sidecar_idle_timer;     /* idle-detection timer handle         */
  uint64_t    sidecar_idle_ms;        /* 0 = disabled (from session conf)    */
  char       *sidecar_tool_use_id;    /* async tool that triggered sidecar   */
  xArray      sidecar_output;         /* accumulated tool output chunks      */
  uint64_t    sidecar_last_output_ms; /* monotonic ms of last chunk        */
};

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Placed in the shared header so query.c can honour it too. */
#define XAGENT_SESSION_DEFAULT_MAX_TURNS 16

/* System prompt used by the internal summary Query when the
 * SummarizeOldest budget policy is active. Instructs the model to
 * produce a concise summary of the conversation segment it receives.
 * The placeholder [N] will be replaced with the number of messages
 * being summarised. */
#define XAGENT_SUMMARY_SYSTEM_PROMPT                                    \
  "Summarise the following %zu messages concisely in no more than 200 " \
  "words. Preserve all names, numbers, decisions and key facts. "       \
  "Do NOT add any information that was not in the original messages."

/* ── Cross-TU helpers (session.c implementers, query.c consumers) ── */

/**
 * @brief Append a (role, text) history entry. @p text is duplicated.
 */
xErrno ai_history_append_text(struct xAgentSession_ *s, xAgentRole role,
                              const char *text, size_t len,
                              int is_summary);

/**
 * @brief Append an Assistant chain-of-thought entry. @p text is duplicated.
 */
xErrno ai_history_append_thinking(struct xAgentSession_ *s, const char *text,
                                  size_t len);

/**
 * @brief Append an Assistant tool_use entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_use(struct xAgentSession_ *s, const char *id,
                                  const char *name, const char *args);

/**
 * @brief Append a Tool tool_result entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_result(struct xAgentSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error);

#endif /* XAGENT_SESSION_PRIVATE_H */
