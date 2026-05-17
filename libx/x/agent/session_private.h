/*
 * Copyright 2025 The libx Authors. All rights reserved.
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

#include "query_private.h"

#include <x/agent/session.h>
#include <x/base/array.h>

#include <stddef.h>

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

  /* ── Configuration snapshot (owned by the session) ──────────── */
  const char *system_prompt; /* borrowed from conf or agent           */
  char       *model;         /* owned: strdup'd at create / SetModel  */
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
  int    known_prompt_tokens; /* -1 = unknown / cold start          */
  size_t delta_entries;       /* entries added since last report     */
  size_t last_gate_total;     /* gate's pre-submit total (or 0)     */

  /* The provider-reported prompt_tokens from the first round of
   * the most recently completed run, or -1 if unavailable.
   * Exposed via xAgentBudgetGateInfo so callers can compare
   * estimated vs actual. */
  int last_first_round_prompt_tokens;

  /* Token overhead that the budget gate must reserve for non-history
   * consumers: system_prompt + memory hits. These are estimated once
   * and cached so the gate doesn't re-estimate on every call.
   * Updated after each view build / prompt submission. */
  size_t overhead_tokens;

  /* ── Compact-in-progress state (Summarize policy) ──────
   *
   * When the budget gate fires on Summarize, the Session
   * launches an internal summary Query that compresses old history
   * into one System entry. During this compaction:
   *   - compacting == 1 signals "compact query in flight";
   *   - compact_start_idx records the inclusive start of the
   *     range that will be replaced by one summary;
   *   - compact_end_idx records the exclusive end of the range
   *     [compact_start_idx..compact_end_idx) that will be
   *     replaced by one summary;
   *   - budget enforcement is implicitly disabled because the
   *     compact Query is driven by session_enforce_budget_ which
   *     gates on s->compacting, preventing recursive budget checks.
   *
   * All are zero when no compact is in progress. */
  int    compacting;        /* 1 = compact query in flight              */
  size_t compact_start_idx; /* inclusive start of range to replace      */
  size_t compact_end_idx;   /* exclusive end of range to replace        */

  /* ── Compact anti-loop guard ─────────────────────────────────────
   *
   * After a compact finishes but the session is still over budget,
   * the auto-retry re-enters session_enforce_budget_. Without a
   * guard this can loop forever:
   *   compact → still over → compact → still over → …
   *
   * last_compact_history_len records the history array length at the
   * time the most recent compact was launched. If a new compact
   * would be launched but the history hasn't grown since the last
   * attempt, refuse with PromptTooLong — another compact won't help.
   *
   * Reset to 0 on gate pass (budget is OK again). */
  size_t last_compact_history_len;

  /* ── Pending message for auto-retry after compact ────────────
   *
   * When a compact is launched, the user's input message is saved
   * here. After compact completes, the session automatically
   * retries this message without requiring the caller to re-submit.
   * pending_text is a deep copy (session-owned); freed on destroy
   * or when a new compact replaces it. */
  char  *pending_text;
  size_t pending_text_len;

  /* ── Session-lifetime properties (stamped at create, immutable) ── */
  xAgentInputOrigin           origin;           /* default User on zero    */
  xAgentSessionFinalizingFunc on_finalizing;    /* NULL = no hook          */
  void                       *finalizing_owner; /* passed back verbatim    */

  /* Memory store for persistence. Borrowed from the agent at
   * create time; NULL when no store is configured. */
  xAgentMemory memory;

  /* Owned copy of session_id used for memory store writes.
   * Survives session teardown so Finalizing append can reference
   * it. Freed in xAgentSessionDestroy after the Finalizing
   * append completes. */
  char *session_id_copy;

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

  struct xAgentQuery_ *sidecar;                /* NULL = no sidecar active            */
  xEventTimer          sidecar_idle_timer;     /* idle-detection timer handle         */
  uint64_t             sidecar_idle_ms;        /* 0 = disabled (from session conf)    */
  char                *sidecar_tool_use_id;    /* async tool that triggered sidecar   */
  xArray               sidecar_output;         /* accumulated tool output chunks      */
  uint64_t             sidecar_last_output_ms; /* monotonic ms of last chunk        */
};

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Placed in the shared header so query.c can honour it too. */
#define XAGENT_SESSION_DEFAULT_MAX_TURNS 16

/* System prompt used by the internal summary Query when the
 * Summarize budget policy is active. Instructs the model to
 * produce a concise summary of the conversation segment it receives.
 * The placeholder [N] will be replaced with the number of messages
 * being summarised. */
#define XAGENT_SUMMARY_INSTRUCT_PROMPT                                  \
  "The conversation above has grown too long and must be compressed. "  \
  "Please summarise the %zu messages above so that the conversation "   \
  "can continue with this summary in place of the originals. Be as "    \
  "concise as possible while preserving all names, numbers, decisions " \
  "and key facts. Do NOT add any information that was not in the "      \
  "original messages."

/* ── Cross-TU helpers (session.c implementers, query.c consumers) ── */

/**
 * @brief Append a (role, text) history entry. @p text is duplicated.
 */
xErrno ai_history_append_text(struct xAgentSession_ *s, xAgentRole role, const char *text,
                              size_t len);

/**
 * @brief Append an Assistant chain-of-thought entry. @p text is duplicated.
 */
xErrno ai_history_append_thinking(struct xAgentSession_ *s, const char *text, size_t len);

/**
 * @brief Append an Assistant tool_use entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_use(struct xAgentSession_ *s, const char *id, const char *name,
                                  const char *args);

/**
 * @brief Append a Tool tool_result entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_result(struct xAgentSession_ *s, const char *id, const char *output,
                                     size_t output_len, int is_error);

#endif /* XAGENT_SESSION_PRIVATE_H */
