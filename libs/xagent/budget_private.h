/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_private.h - Context-budget helpers for xAgentSession.
 *
 * Scope
 * -----
 * Pure, side-effect-free utilities that support the xAgentBudgetConf
 * enforcement path. Wired into xAgentSessionInput and
 * sess_fwd_on_done by session.c; this header only publishes the
 * pieces, not the policy decisions.
 *
 * Two concerns, two functions, no cross-talk:
 *
 *   1. ai_budget_estimate_tokens() — how big is this sequence of
 *      entries when the provider sees it, roughly? Returns an
 *      approximate token count from a static bytes/4 + envelope
 *      formula.
 *
 *   2. ai_budget_find_user_turn() — given a history slice, at
 *      what index does the k-th User-role entry sit? Pure indexing
 *      utility, used both by the trimmer and by tests to describe
 *      expected boundaries readably.
 *
 * Exposed only to session.c, budget.c and budget_test.cpp. Other
 * xai translation units must go through session.h.
 */

#ifndef XAGENT_BUDGET_PRIVATE_H
#define XAGENT_BUDGET_PRIVATE_H

#include "turn_private.h" /* struct xAgentSessionMsg_, xAgentSessionEntry_* */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-entry envelope overhead added on top of the payload
 *        byte count when estimating tokens.
 *
 * Represents the role marker and JSON/wire framing that every
 * provider wraps around each message. 8 is a conservative round
 * number — most providers cost between 3 and 7 real tokens per
 * envelope, so overshooting by ~1 token per entry errs on the side
 * of trimming slightly more aggressively than strictly necessary.
 * Exposed as a macro so tests can reproduce the formula without
 * hard-coding the magic number.
 */
#define XAGENT_BUDGET_PER_MSG_TOKENS 8u

/**
 * @brief Bytes-per-token divisor used by the coarse estimator.
 *
 * The canonical "1 token ≈ 4 bytes of English" heuristic. Not
 * accurate for CJK or for tool_use JSON, but conservative in both
 * directions: overestimates CJK (we trim earlier), underestimates
 * compact JSON by a small margin.
 */
#define XAGENT_BUDGET_BYTES_PER_TOKEN 4u

/**
 * @brief Sentinel returned by ai_budget_find_nth_user_turn() when
 *        the requested turn does not exist.
 *
 * Defined as SIZE_MAX so callers can test with a simple equality
 * check. Indexing functions never return this value for a valid
 * turn because history arrays are bounded by size_t capacity far
 * below SIZE_MAX.
 */
#define XAGENT_BUDGET_NO_SUCH_TURN ((size_t) -1)

/**
 * @brief Built-in @c max_tokens default when the caller sets
 *        @ref xAgentBudgetConf::context_window to zero.
 *
 * 128000 covers the context windows of the models this codebase
 * currently talks to (Claude 3.5 / 3.7, GPT-4o / 4.1, kimi-k2.6,
 * DeepSeek-V3) without overshooting the smallest deployed variant.
 * Callers that know their downstream limit (e.g. gpt-3.5-turbo at
 * 16k or an internal-only 32k Sonnet deployment) MUST set an
 * explicit @c context_window in @ref xAgentBudgetConf; this
 * fallback is chosen for "something sensible" rather than "tight
 * fit".
 */
#define XAGENT_BUDGET_DEFAULT_MAX_TOKENS 128000u

/**
 * @brief Estimate the approximate token count for a flat slice of
 *        turn entries.
 *
 * The estimate is the sum of each entry's payload bytes divided by
 * @ref XAGENT_BUDGET_BYTES_PER_TOKEN, plus @ref XAGENT_BUDGET_PER_MSG_TOKENS
 * per entry for role / framing overhead.
 *
 * Payload bytes per kind:
 *
 *   - Text, Thinking    → @c text_len
 *   - ToolUse           → strlen(tool_use_name) + strlen(tool_use_args)
 *                         (the id is an opaque short nonce and is
 *                          folded into the envelope constant)
 *   - ToolResult        → @c tool_result_output_len
 *
 * NULL @p msgs is valid iff @p n is zero and returns zero.
 *
 * @param msgs  Array of entries. Entries are read but never mutated.
 * @param n     Number of entries at @p msgs.
 * @return      Approximate token count. Never negative. For @p n ==
 *              0, returns 0 (no envelope tax without any entries).
 */
size_t ai_budget_estimate_tokens(const struct xAgentSessionMsg_ *msgs, size_t n);

/**
 * @brief Return the index of the @p k-th User-role entry in the
 *        slice (0-indexed).
 *
 * Scans forward from the start. Only entries with
 * @c role == xAgentRole_User are counted; Assistant / Tool entries are
 * skipped over regardless of their @c kind.
 *
 * @param msgs  Entry array.
 * @param n     Number of entries at @p msgs.
 * @param k     Which User-role entry to locate (0 = first).
 * @return      The index of the k-th User-role entry, or
 *              @ref XAGENT_BUDGET_NO_SUCH_TURN if the slice contains
 *              fewer than @p k + 1 User-role entries.
 */
size_t ai_budget_find_user_turn(const struct xAgentSessionMsg_ *msgs, size_t n,
                                size_t k);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XAGENT_BUDGET_PRIVATE_H */
