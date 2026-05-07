/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_private.h - Context-budget helpers for xAgentSession.
 *
 * Scope
 * -----
 * Pure, side-effect-free utilities plus a tiny online calibrator
 * that support the xAgentBudgetConf enforcement path from
 * libs/xai/TODO.md §6. Wired into xAgentSessionInput and
 * sess_fwd_on_done by session.c as of c3/c4; this header only
 * publishes the pieces, not the policy decisions.
 *
 * Three concerns, three functions, no cross-talk:
 *
 *   1. ai_budget_estimate_tokens() — how big is this sequence of
 *      entries when the provider sees it, roughly? Returns an
 *      approximate token count from a static bytes/4 + envelope
 *      formula. ai_budget_estimate_tokens_calibrated() layers an
 *      online multiplier on top; the calibrator state that feeds
 *      that multiplier lives on the session and is updated from
 *      provider-reported xAgentUsage in sess_fwd_on_done.
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
 *      spelled out in xAgentBudgetPolicy's doc.
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
 * compact JSON by a small margin. The calibrator layered on top
 * of this static estimate (see ai_budget_calibrator_update)
 * corrects the resulting systematic bias per session.
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
 *        @ref xAgentBudgetConf::max_tokens to zero.
 *
 * 128000 covers the context windows of the models this codebase
 * currently talks to (Claude 3.5 / 3.7, GPT-4o / 4.1, kimi-k2.6,
 * DeepSeek-V3) without overshooting the smallest deployed variant.
 * Callers that know their downstream limit (e.g. gpt-3.5-turbo at
 * 16k or an internal-only 32k Sonnet deployment) MUST set an
 * explicit @c max_tokens in @ref xAgentBudgetConf; this fallback is
 * chosen for "something sensible" rather than "tight fit".
 */
#define XAGENT_BUDGET_DEFAULT_MAX_TOKENS 128000u

/**
 * @brief Default maximum length (in bytes) for a single tool_result
 *        entry before it gets truncated in-place.
 *
 * When a tool_result exceeds this threshold, the output is truncated
 * and a "[truncated: showing N/M bytes]" marker is appended. Zero
 * means no truncation. Configurable per session via
 * @ref xAgentBudgetConf::max_tool_result_bytes.
 */
#define XAGENT_BUDGET_DEFAULT_MAX_TOOL_RESULT_BYTES 8192u

/**
 * @brief Default @c trim_tool_results_threshold: 0 = disabled.
 *
 * When non-zero, the value is stored as percentage × 100
 * (e.g. 7000 = 70%). See @ref xAgentBudgetConf::trim_tool_results_threshold.
 */
#define XAGENT_BUDGET_DEFAULT_TRIM_TOOL_RESULTS_THRESHOLD 0

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
size_t ai_budget_find_nth_user_turn(const struct xAgentSessionMsg_ *msgs, size_t n,
                                    size_t k);

/**
 * @brief Compute the earliest index the session may keep from
 *        without violating any budget invariant.
 *
 * Returned value @c idx has this meaning: entries in @c msgs[0..idx)
 * are safe to drop, entries in @c msgs[idx..n) must be retained.
 * A return value of @c 0 therefore means "nothing may be dropped".
 *
 * Guarantees (matching the invariants in xAgentBudgetPolicy's doc,
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
 * @param msgs               Entry array (typically xAgentSession_::history).
 * @param n                  Number of entries at @p msgs.
 * @param keep_recent_turns  Minimum number of complete recent User
 *                           turns that must remain intact.
 * @return Index where retention must begin. In @c [0, n].
 */
size_t ai_budget_earliest_keep(const struct xAgentSessionMsg_ *msgs, size_t n,
                               size_t keep_recent_turns);

/**
 * @brief Compute the fraction of token cost attributable to tool-use
 *        / tool-result entries in the given slice.
 *
 * Returns a value in [0.0, 1.0] where 1.0 means every token in the
 * slice comes from ToolUse or ToolResult entries, and 0.0 means none
 * of them do (pure text / thinking conversation). Used by the Auto
 * budget policy to decide whether SummarizeOldest (good for text) or
 * TruncateTail (safer for structured tool data) is the better
 * strategy.
 *
 * The ratio is weighted by estimated token cost (payload + envelope),
 * not by raw entry count — a single 2 KiB tool_result should count
 * more than three 10-byte text entries.
 *
 * Returns 0.0 for empty inputs.
 *
 * @param msgs  Entry array.
 * @param n     Number of entries at @p msgs.
 * @return      Tool-entry token ratio in [0.0, 1.0].
 */
double ai_budget_tool_ratio(const struct xAgentSessionMsg_ *msgs, size_t n);

/**
 * @brief Compute the tail-truncation boundary that preserves the
 *        prompt-cache prefix.
 *
 * For TruncateTail (cache-friendly truncation): instead of removing
 * the oldest entries (which changes the prompt prefix and invalidates
 * provider-side prompt caching), we remove entries from the tail end
 * of history. This keeps the prefix stable so the provider's cache
 * stays hot.
 *
 * Returned value @c idx has this meaning: entries in @c msgs[0..idx)
 * must be retained (prefix — never touched, preserves cache), and
 * entries in @c msgs[idx..n) may be dropped. A return value of @c n
 * means "nothing may be dropped" (the entire history is prefix).
 *
 * The function guarantees:
 *   1. At least @p keep_prefix_turns complete User turns are kept
 *      in the @c msgs[0..idx) window (the prefix that survives).
 *      "User turn" = one User entry + every Assistant/Tool entry
 *      that follows before the next User entry.
 *   2. The returned index always lands on a User-role boundary or
 *      equals @p n. This guarantees tool_use/tool_result pairs are
 *      never split.
 *   3. If the slice contains fewer than @p keep_prefix_turns User
 *      entries, returns @p n (keep everything).
 *   4. @p keep_prefix_turns == 0 is legal: only the first User
 *      turn's starting index must survive. If no User entries exist,
 *      returns @p n unconditionally.
 *
 * @param msgs                Entry array.
 * @param n                   Number of entries at @p msgs.
 * @param keep_prefix_turns   Minimum number of User turns to keep as
 *                            prefix (counted from the head / oldest).
 * @return Index beyond which entries may be dropped. In @c [0, n].
 */
size_t ai_budget_tail_keep(const struct xAgentSessionMsg_ *msgs, size_t n,
                           size_t keep_prefix_turns);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XAGENT_BUDGET_PRIVATE_H */
