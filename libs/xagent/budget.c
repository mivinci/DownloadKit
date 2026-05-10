/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget.c - Context-budget helper implementations.
 *
 * Scope discipline: pure functions only. No allocations, no IO,
 * no mutation of caller state. See budget_private.h for the
 * contract each function must honour and the reasoning behind the
 * constants.
 */

#include "budget_private.h"

#include <xagent/message.h> /* xAgentRole_User */

#include <string.h>

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_estimate_tokens
 *
 * Coarse "bytes / 4 plus per-message envelope" estimator. The
 * per-kind branch is deliberately verbose: each case reads the
 * exact fields that ai_history_append_*() populates for that kind,
 * which keeps this function in lock-step with turn_private.h —
 * when a new entry kind is added there, the compiler warning on an
 * unhandled switch case will flag this function first.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_estimate_tokens(const struct xAgentSessionMsg_ *msgs, size_t n) {
  if (!msgs || n == 0) return 0;

  size_t payload_bytes = 0;

  for (size_t i = 0; i < n; ++i) {
    const struct xAgentSessionMsg_ *m = &msgs[i];

    switch (m->kind) {
    case xAgentSessionEntry_Text:
    case xAgentSessionEntry_Thinking:
      payload_bytes += m->text_len;
      break;

    case xAgentSessionEntry_ToolUse:
      /* Name and args are guaranteed non-NULL on populated
       * entries (see ai_history_append_tool_use), but we guard
       * defensively so this function stays safe on partially
       * constructed fixtures in tests. */
      if (m->tool_use_name) payload_bytes += strlen(m->tool_use_name);
      if (m->tool_use_args) payload_bytes += strlen(m->tool_use_args);
      break;

    case xAgentSessionEntry_ToolResult:
      payload_bytes += m->tool_result_output_len;
      break;
    }
  }

  return (payload_bytes / XAGENT_BUDGET_BYTES_PER_TOKEN) +
         (n * XAGENT_BUDGET_PER_MSG_TOKENS);
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_find_user_turn
 *
 * Return the array index of the @p k-th User-role entry (0-based).
 * Linear scan; slice sizes are in the tens-to-low-hundreds range,
 * so this is never a hot path.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_find_user_turn(const struct xAgentSessionMsg_ *msgs, size_t n,
                                size_t k) {
  if (!msgs) return XAGENT_BUDGET_NO_SUCH_TURN;

  size_t seen = 0;
  for (size_t i = 0; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) {
      if (seen == k) return i;
      ++seen;
    }
  }
  return XAGENT_BUDGET_NO_SUCH_TURN;
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_recent_band_start
 *
 * Return the start index of the "recent band" — the slice of
 * history that must be kept because it contains the newest
 * @p keep_recent_turns user turns.
 *
 *   msgs[0 .. idx)           → droppable (older than recent band)
 *   msgs[idx .. n)           → recent band (must be kept)
 *
 * Returns 0 when nothing may be dropped (entire history is recent).
 *
 * The returned index is always either 0 or the index of a User-role
 * entry.  This guarantees tool_use / tool_result pairs are never
 * split: history is built by ai_history_append_*() which only
 * places ToolUse / ToolResult under Assistant / Tool roles, so a
 * User-role boundary can never fall inside a tool pair.
 *
 * keep_recent_turns == 0 means "no mandatory retention floor", but
 * the boundary must still land on a User entry (tool-pair
 * invariant).  We collapse to the last User entry — effectively
 * keeping only the most recent turn, which is the tightest
 * boundary the invariant allows.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_recent_band_start(const struct xAgentSessionMsg_ *msgs, size_t n,
                                   size_t keep_recent_turns) {
  if (!msgs || n == 0) return 0;

  size_t user_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) ++user_count;
  }

  if (user_count == 0) return 0;

  /* keep_recent_turns == 0: no mandatory floor, but the boundary
   * must still land on a User entry.  Use the last one — the
   * tightest valid split. */
  if (keep_recent_turns == 0) {
    size_t last = ai_budget_find_user_turn(msgs, n, user_count - 1);
    return last == XAGENT_BUDGET_NO_SUCH_TURN ? 0 : last;
  }

  /* Not enough user turns to meet the floor → keep everything. */
  if (user_count <= keep_recent_turns) return 0;

  /* Normal case: the recent band starts at the
   * (user_count - keep_recent_turns)-th User entry.
   * E.g. 5 user turns, keep_recent = 2 → start at k=3 (4th User). */
  size_t k    = user_count - keep_recent_turns;
  size_t keep = ai_budget_find_user_turn(msgs, n, k);
  return keep == XAGENT_BUDGET_NO_SUCH_TURN ? 0 : keep;
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_head_band_end
 *
 * Return the end index of the "head band" — the slice of history
 * that must be kept as a cache-friendly prefix because it contains
 * the oldest @p keep_prefix_turns user turns.
 *
 *   msgs[0 .. idx)           → head band (must be kept — cache prefix)
 *   msgs[idx .. n)           → droppable (newer than head band)
 *
 * Returns n when nothing may be dropped (entire history is head).
 *
 * The returned index always lands on a User-role entry or equals n,
 * so tool_use / tool_result pairs are never split.
 *
 * keep_prefix_turns == 0 means "no mandatory prefix" → return 0
 * (the head band is empty; everything can be dropped from the
 * tail side).
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_head_band_end(const struct xAgentSessionMsg_ *msgs, size_t n,
                               size_t keep_prefix_turns) {
  if (!msgs || n == 0) return n;

  /* No mandatory prefix → head band is empty. */
  if (keep_prefix_turns == 0) return 0;

  size_t user_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) ++user_count;
  }

  if (user_count == 0) return n;

  /* Not enough user turns to honour the floor → keep everything. */
  if (user_count <= keep_prefix_turns) return n;

  /* Find the last User entry that must be kept (the
   * keep_prefix_turns-th one, 0-indexed → index k-1). Then
   * scan forward for the next User entry — that's where the
   * head band ends and droppable territory begins.
   *
   * E.g. 5 user turns, keep_prefix = 2 → keep User #0 and #1,
   * boundary = start of User #2. */
  size_t last_keep = ai_budget_find_user_turn(msgs, n,
                                               keep_prefix_turns - 1);
  if (last_keep == XAGENT_BUDGET_NO_SUCH_TURN) return n;

  for (size_t i = last_keep + 1; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) return i;
  }

  /* No more User entries after last_keep → head band extends to
   * end of history, nothing can be dropped. */
  return n;
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_tool_ratio
 *
 * Weighted ratio: how much of the estimated token cost comes from
 * ToolUse / ToolResult entries? Uses the same per-kind byte formula
 * as ai_budget_estimate_tokens so the ratio is consistent with what
 * the gate sees.
 * ──────────────────────────────────────────────────────────────── */
double ai_budget_tool_ratio(const struct xAgentSessionMsg_ *msgs, size_t n) {
  if (!msgs || n == 0) return 0.0;

  size_t total_bytes = 0;
  size_t tool_bytes  = 0;

  for (size_t i = 0; i < n; ++i) {
    const struct xAgentSessionMsg_ *m = &msgs[i];

    switch (m->kind) {
    case xAgentSessionEntry_Text:
    case xAgentSessionEntry_Thinking:
      total_bytes += m->text_len;
      break;

    case xAgentSessionEntry_ToolUse: {
      size_t sz = 0;
      if (m->tool_use_name) sz += strlen(m->tool_use_name);
      if (m->tool_use_args) sz += strlen(m->tool_use_args);
      total_bytes += sz;
      tool_bytes  += sz;
      break;
    }

    case xAgentSessionEntry_ToolResult:
      total_bytes += m->tool_result_output_len;
      tool_bytes  += m->tool_result_output_len;
      break;
    }
  }

  if (total_bytes == 0) return 0.0;
  return (double)tool_bytes / (double)total_bytes;
}
