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
