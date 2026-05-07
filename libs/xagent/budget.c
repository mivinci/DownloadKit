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
 * ai_budget_find_nth_user_turn
 *
 * Linear scan. The slice sizes we deal with are in the tens to low
 * hundreds, so this is not a hot path even when called repeatedly
 * from the trimmer.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_find_nth_user_turn(const struct xAgentSessionMsg_ *msgs, size_t n,
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
 * ai_budget_earliest_keep
 *
 * High-level shape:
 *
 *   1. Count total User-role entries in the slice (call it @c U).
 *   2. If U == 0, nothing anchors a trim boundary → return 0.
 *   3. If U <= keep_recent_turns, we do not have enough history to
 *      meet the floor → return 0 (the caller keeps everything).
 *   4. Otherwise the keep window begins at the (U - keep_recent_turns)-th
 *      User-role entry (0-indexed), i.e. the first User turn that
 *      we are obliged to keep. Everything strictly before that
 *      index is safe to drop.
 *
 * Correctness sketch for the tool_use / tool_result invariant:
 * history is produced exclusively by ai_history_append_*(). User
 * messages (xAgentRole_User) can only carry Text entries — never
 * ToolUse or ToolResult. Therefore a User-role boundary can never
 * sit between a tool_use and its matching tool_result, so slicing
 * the array at that index preserves pairing.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_earliest_keep(const struct xAgentSessionMsg_ *msgs, size_t n,
                               size_t keep_recent_turns) {
  if (!msgs || n == 0) return 0;

  /* Count user turns in one pass. Cheap and keeps the subsequent
   * find_nth_user_turn call from having to walk past the desired
   * entry just to verify it exists. */
  size_t user_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) ++user_count;
  }

  if (user_count == 0) return 0;

  /* Special-case keep_recent_turns == 0: keep only the last user
   * turn. The earliest-keep index is then the index of the last
   * User-role entry, because everything strictly before it (its
   * older peers plus their assistant/tool chatter) is expendable. */
  if (keep_recent_turns == 0) {
    size_t last = ai_budget_find_nth_user_turn(msgs, n, user_count - 1);
    /* user_count > 0 here, so find_nth_user_turn must succeed. */
    return last == XAGENT_BUDGET_NO_SUCH_TURN ? 0 : last;
  }

  /* Not enough history to honour the floor: keep everything. */
  if (user_count <= keep_recent_turns) return 0;

  /* The keep window starts at the (user_count - keep_recent_turns)-th
   * User-role entry. Example: 5 user turns, keep_recent_turns = 2
   * → start keeping from the 3rd user turn (index 2 in 0-based k). */
  size_t k    = user_count - keep_recent_turns;
  size_t keep = ai_budget_find_nth_user_turn(msgs, n, k);
  return keep == XAGENT_BUDGET_NO_SUCH_TURN ? 0 : keep;
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_tail_keep
 *
 * Cache-friendly truncation boundary: instead of removing the oldest
 * entries (which changes the prompt prefix and invalidates prompt
 * caching), we compute how far from the HEAD we must keep to satisfy
 * the keep_prefix_turns floor. Everything beyond that boundary may
 * be dropped from the tail.
 *
 * High-level shape:
 *
 *   1. Count total User-role entries in the slice (call it @c U).
 *   2. If U == 0, nothing anchors a boundary → return n (keep all).
 *   3. If U <= keep_prefix_turns, we need to keep everything to
 *      honour the floor → return n.
 *   4. Otherwise find the END of the keep_prefix_turns-th User turn
 *      group. The index just past that group is the boundary;
 *      entries beyond it (newer turns) may be dropped.
 *
 * "End of the keep_prefix_turns-th User turn group" means the index
 * of the (keep_prefix_turns)-th User entry plus all its trailing
 * Assistant/Tool entries. Concretely: we find the
 * (keep_prefix_turns)-th User entry, then find the NEXT User entry
 * after it; the boundary is the next User entry's index (or n if
 * there is no next User entry).
 *
 * Correctness for tool_use/tool_result pairing: the boundary always
 * lands on a User-role entry (or n), so no tool pair is split.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_tail_keep(const struct xAgentSessionMsg_ *msgs, size_t n,
                           size_t keep_prefix_turns) {
  if (!msgs || n == 0) return n;

  /* Count user turns in one pass. */
  size_t user_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) ++user_count;
  }

  if (user_count == 0) return n;

  /* Not enough history to honour the floor: keep everything. */
  if (user_count <= keep_prefix_turns) return n;

  /* Special-case keep_prefix_turns == 0: keep only the first user
   * turn group. The boundary is the start of the 2nd User entry. */
  if (keep_prefix_turns == 0) {
    /* Find the first User entry, then the second User entry. */
    size_t first = ai_budget_find_nth_user_turn(msgs, n, 0);
    if (first == XAGENT_BUDGET_NO_SUCH_TURN) return n;
    /* Scan forward from first+1 to find the next User entry. */
    for (size_t i = first + 1; i < n; ++i) {
      if (msgs[i].role == xAgentRole_User) return i;
    }
    /* No second User entry — the first turn group extends to end. */
    return n;
  }

  /* Find the (keep_prefix_turns)-th User entry (0-indexed). This is
   * the last User entry we must keep. Everything after its turn group
   * may be dropped. */
  size_t last_keep = ai_budget_find_nth_user_turn(msgs, n,
                                                   keep_prefix_turns - 1);
  if (last_keep == XAGENT_BUDGET_NO_SUCH_TURN) return n;

  /* Find the next User entry after last_keep — that's the boundary. */
  for (size_t i = last_keep + 1; i < n; ++i) {
    if (msgs[i].role == xAgentRole_User) return i;
  }

  /* No more User entries after last_keep: the turn group extends to
   * the end of history — nothing can be dropped. */
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
