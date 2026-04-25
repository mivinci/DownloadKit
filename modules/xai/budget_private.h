/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_private.h - Context-budget helpers for xAiSession.
 *
 * Scope
 * -----
 * Pure, side-effect-free utilities that support the context_budget
 * enforcement path introduced in modules/xai/TODO.md §6. None of
 * these functions are wired into xAiSessionInput yet — that is c3's
 * job. c2 only provides the building blocks and their unit tests.
 *
 * Three concerns, three functions, no cross-talk:
 *
 *   1. ai_budget_estimate_tokens() — how big is this sequence of
 *      entries when the provider sees it, roughly? Returns an
 *      approximate token count. Deliberately coarse; calibration
 *      against real xAiUsage lands in c4.
 *
 *   2. ai_budget_find_nth_user_turn() — given a history slice, at
 *      what index does the k-th User-role entry sit? Pure indexing
 *      utility, used both by the trimmer and by tests to describe
 *      expected boundaries readably.
 *
 *   3. ai_budget_earliest_keep() — given a history and a
 *      keep_recent_turns lower bound, what is the earliest index
 *      the session is allowed to start keeping from? In other
 *      words: everything strictly before the returned index may
 *      be dropped without violating any of the four invariants
 *      spelled out in xAiBudgetPolicy's doc.
 *
 * Exposed only to session.c, budget.c and budget_test.cpp. Other
 * xai translation units must go through session.h.
 */

#ifndef XAI_BUDGET_PRIVATE_H
#define XAI_BUDGET_PRIVATE_H

#include "turn_private.h" /* struct xAiSessionMsg_, xAiSessionEntry_* */

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
#define XAI_BUDGET_PER_MSG_TOKENS 8u

/**
 * @brief Bytes-per-token divisor used by the coarse estimator.
 *
 * The canonical "1 token ≈ 4 bytes of English" heuristic. Not
 * accurate for CJK or for tool_use JSON, but conservative in both
 * directions: overestimates CJK (we trim earlier), underestimates
 * compact JSON by a small margin (c4 calibration will tighten this
 * once we have live xAiUsage feedback to compare against).
 */
#define XAI_BUDGET_BYTES_PER_TOKEN 4u

/**
 * @brief Sentinel returned by ai_budget_find_nth_user_turn() when
 *        the requested turn does not exist.
 *
 * Defined as SIZE_MAX so callers can test with a simple equality
 * check. Indexing functions never return this value for a valid
 * turn because history arrays are bounded by size_t capacity far
 * below SIZE_MAX.
 */
#define XAI_BUDGET_NO_SUCH_TURN ((size_t)-1)

/**
 * @brief Estimate the approximate token count for a flat slice of
 *        turn entries.
 *
 * The estimate is the sum of each entry's payload bytes divided by
 * @ref XAI_BUDGET_BYTES_PER_TOKEN, plus @ref XAI_BUDGET_PER_MSG_TOKENS
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
size_t ai_budget_estimate_tokens(const struct xAiSessionMsg_ *msgs, size_t n);

/**
 * @brief Return the index of the @p k-th User-role entry in the
 *        slice (0-indexed).
 *
 * Scans forward from the start. Only entries with
 * @c role == xAiRole_User are counted; Assistant / Tool entries are
 * skipped over regardless of their @c kind.
 *
 * @param msgs  Entry array.
 * @param n     Number of entries at @p msgs.
 * @param k     Which User-role entry to locate (0 = first).
 * @return      The index of the k-th User-role entry, or
 *              @ref XAI_BUDGET_NO_SUCH_TURN if the slice contains
 *              fewer than @p k + 1 User-role entries.
 */
size_t ai_budget_find_nth_user_turn(const struct xAiSessionMsg_ *msgs, size_t n,
                                    size_t k);

/**
 * @brief Compute the earliest index the session may keep from
 *        without violating any budget invariant.
 *
 * Returned value @c idx has this meaning: entries in @c msgs[0..idx)
 * are safe to drop, entries in @c msgs[idx..n) must be retained.
 * A return value of @c 0 therefore means "nothing may be dropped".
 *
 * Guarantees (matching the invariants in xAiBudgetPolicy's doc,
 * with system_prompt handled outside of history — see session.c):
 *
 *   1. The returned index is always either 0 or the index of a
 *      User-role entry. This guarantees tool_use / tool_result
 *      pairs are never cut in half: a User-role entry can never
 *      be preceded by an unmatched tool_result in history.
 *   2. At least @p keep_recent_turns complete User turns are kept
 *      in the @c msgs[idx..n) window. A "User turn" counts one
 *      User-role entry plus every Assistant/Tool entry that
 *      follows before the next User-role entry.
 *   3. If the slice contains strictly fewer than @p keep_recent_turns
 *      User-role entries, the function returns 0 (keep everything)
 *      — there is not enough history to honour the floor, so we
 *      prefer safety over compliance.
 *   4. @p keep_recent_turns == 0 is legal: the earliest trimmable
 *      boundary collapses to the last User-role entry's index
 *      (keep only the current-most turn). If the slice has no
 *      User-role entries at all, returns 0 unconditionally.
 *
 * This function is pure policy computation; it does not consult
 * any token budget. The actual "should I trim, and how much?"
 * decision lives in c3 and layers on top of this result.
 *
 * @param msgs               Entry array (typically xAiSession_::history).
 * @param n                  Number of entries at @p msgs.
 * @param keep_recent_turns  Minimum number of complete recent User
 *                           turns that must remain intact.
 * @return Index where retention must begin. In @c [0, n].
 */
size_t ai_budget_earliest_keep(const struct xAiSessionMsg_ *msgs, size_t n,
                               size_t keep_recent_turns);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XAI_BUDGET_PRIVATE_H */
