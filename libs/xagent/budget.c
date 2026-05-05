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
 * ai_budget_estimate_tokens_calibrated
 *
 * Thin adapter over the raw estimator: compute once, multiply,
 * round to nearest. Callers that need the uncalibrated answer go
 * through ai_budget_estimate_tokens() directly; we do not publish
 * a "pass 1.0 to this" convenience because the clarity of the
 * function name at the call site is worth more than saving a
 * branch.
 *
 * Rounding: +0.5 before truncation. For the clamp range this is
 * well-behaved — raw * 2.0 on a size_t that fits in a history of
 * reasonable size cannot overflow IEEE-754 double precision.
 * ──────────────────────────────────────────────────────────────── */
size_t ai_budget_estimate_tokens_calibrated(const struct xAgentSessionMsg_ *msgs,
                                            size_t n, double factor) {
  size_t raw = ai_budget_estimate_tokens(msgs, n);
  if (raw == 0) return 0;
  double adjusted = (double)raw * factor + 0.5;
  /* Safety net: a pathologically negative factor would go to 0
   * after truncation. We document the input range in the header
   * but still want a defined answer if a caller violates it. */
  if (adjusted < 0.0) return 0;
  return (size_t)adjusted;
}

/* ─────────────────────────────────────────────────────────────────
 * ai_budget_calibrator_init / _update
 *
 * Tiny stateful pair — the only piece of non-pure code in this
 * translation unit. Kept here rather than in session.c so the full
 * arithmetic (EWMA step + clamp + opt-out rules) is co-located
 * with its tests in budget_test.cpp and with the constants it
 * references from budget_private.h.
 * ──────────────────────────────────────────────────────────────── */
void ai_budget_calibrator_init(xAgentBudgetCalibrator *c) {
  if (!c) return;
  c->factor  = 1.0;
  c->samples = 0;
}

void ai_budget_calibrator_update(xAgentBudgetCalibrator *c, size_t estimated,
                                 int actual) {
  if (!c) return;
  if (estimated == 0) return; /* would divide by zero */
  if (actual <= 0)    return; /* provider signalled "unknown" */

  double observed = (double)actual / (double)estimated;
  double next     = (1.0 - XAGENT_BUDGET_CALIBRATION_ALPHA) * c->factor +
                    XAGENT_BUDGET_CALIBRATION_ALPHA * observed;

  if (next < XAGENT_BUDGET_CALIBRATION_MIN_FACTOR) {
    next = XAGENT_BUDGET_CALIBRATION_MIN_FACTOR;
  } else if (next > XAGENT_BUDGET_CALIBRATION_MAX_FACTOR) {
    next = XAGENT_BUDGET_CALIBRATION_MAX_FACTOR;
  }
  c->factor = next;

  if (c->samples != (size_t)-1) c->samples++;
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
