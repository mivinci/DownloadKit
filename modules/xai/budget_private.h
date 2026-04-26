/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_private.h - Context-budget helpers for xAiSession.
 *
 * Scope
 * -----
 * Pure, side-effect-free utilities plus a tiny online calibrator
 * that support the xAiBudgetConf enforcement path from
 * modules/xai/TODO.md §6. Wired into xAiSessionInput and
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
 *      provider-reported xAiUsage in sess_fwd_on_done.
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
 * compact JSON by a small margin. The calibrator layered on top
 * of this static estimate (see ai_budget_calibrator_update)
 * corrects the resulting systematic bias per session.
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
#define XAI_BUDGET_NO_SUCH_TURN ((size_t) -1)

/**
 * @brief Built-in @c max_tokens default when the caller sets
 *        @ref xAiBudgetConf::max_tokens to zero.
 *
 * 128000 covers the context windows of the models this codebase
 * currently talks to (Claude 3.5 / 3.7, GPT-4o / 4.1, kimi-k2.6,
 * DeepSeek-V3) without overshooting the smallest deployed variant.
 * Callers that know their downstream limit (e.g. gpt-3.5-turbo at
 * 16k or an internal-only 32k Sonnet deployment) MUST set an
 * explicit @c max_tokens in @ref xAiBudgetConf; this fallback is
 * chosen for "something sensible" rather than "tight fit".
 */
#define XAI_BUDGET_DEFAULT_MAX_TOKENS 128000u

/**
 * @name Token-estimate calibration constants
 *
 * These drive the post-run feedback loop that adjusts the coarse
 * static estimator using provider-reported @ref xAiUsage values.
 * The loop is deliberately conservative: a small EWMA step, hard
 * clamps at both ends, and opt-out rules for rounds we cannot
 * attribute cleanly (multi-round tool loops). See
 * ai_budget_calibrator_update() for the exact rules.
 * @{
 */

/**
 * @brief EWMA weight for each new observation.
 *
 * On every accepted observation the factor updates as
 *
 *   factor := (1 - ALPHA) * factor + ALPHA * observed
 *
 * 0.25 means "a fresh sample pulls the factor a quarter of the
 * way toward itself". This converges in ~5–10 rounds on a stable
 * workload but is slow enough that a single outlier (e.g. an
 * unusually long tool_result) does not dominate the next gate
 * decision. Values larger than 0.5 start feeling like a moving
 * assignment rather than a smoothed average; smaller than 0.1
 * converge too slowly for interactive sessions.
 */
#define XAI_BUDGET_CALIBRATION_ALPHA 0.25

/**
 * @brief Hard lower and upper clamps on the calibrated factor.
 *
 * Defends the gate against two failure modes:
 *
 *   - Provider reports @c prompt_tokens that excludes the system
 *     prompt or compresses whitespace aggressively, making the
 *     true/estimate ratio drift below 0.5. Letting the factor go
 *     arbitrarily small would make the gate too permissive and
 *     eventually hand the provider a request it actually can't
 *     serve.
 *   - A bug on either side (tokenizer change, a single malformed
 *     response with a bogus usage block, a switch to a CJK-heavy
 *     workload right after an English one) pushes the ratio
 *     momentarily to absurd highs. Clamping at 2.0 means the
 *     worst a single bad observation can do is double our
 *     estimate for a round or two, not explode it.
 *
 * Real-world ratios on sane models sit in [0.7, 1.3]; the ±0.5
 * range around 1.0 is headroom, not a target.
 */
#define XAI_BUDGET_CALIBRATION_MIN_FACTOR 0.5
#define XAI_BUDGET_CALIBRATION_MAX_FACTOR 2.0

/** @} */

/**
 * @brief Online token-estimate calibrator.
 *
 * A tiny bit of state that rides on each xAiSession_ and lets the
 * budget gate correct for systematic bias in the static estimator.
 * Every successful provider round with clean attribution (see
 * ai_budget_calibrator_update()) produces one observation; the
 * EWMA-smoothed factor is then consulted by
 * ai_budget_estimate_tokens_calibrated() on the next gate check.
 *
 * Fields:
 *   - @c factor:    current multiplier applied to raw estimates.
 *                   Initialised to 1.0 (identity) so a brand-new
 *                   session with no observations behaves exactly
 *                   like the pre-c4 static estimator.
 *   - @c samples:   monotonic observation counter, for tests and
 *                   diagnostics. Saturates at SIZE_MAX so it never
 *                   wraps in a long-lived session.
 *
 * The struct is intentionally POD / trivially copyable — no heap,
 * no pointers, no destructor needed. Zero-initialisation is NOT
 * valid (factor would be 0.0 and the gate would think every
 * estimate is tiny); always go through ai_budget_calibrator_init().
 */
typedef struct xAiBudgetCalibrator_ {
  double factor;  /* current EWMA-smoothed multiplier             */
  size_t samples; /* count of accepted observations (saturating)  */
} xAiBudgetCalibrator;

/**
 * @brief Reset a calibrator to identity (factor = 1.0, samples = 0).
 *
 * Must be called once per xAiSession_ during create. Safe to call
 * on a previously-initialised calibrator (discards prior state).
 */
void ai_budget_calibrator_init(xAiBudgetCalibrator *c);

/**
 * @brief Fold one observation into the calibrator.
 *
 * Computes @c observed = (double) actual / estimated, applies one
 * EWMA step with @ref XAI_BUDGET_CALIBRATION_ALPHA, then clamps
 * the result into
 * [@ref XAI_BUDGET_CALIBRATION_MIN_FACTOR,
 *  @ref XAI_BUDGET_CALIBRATION_MAX_FACTOR].
 *
 * Opt-out rules (observation ignored, state unchanged, @c samples
 * NOT incremented):
 *
 *   - @p c is NULL.
 *   - @p estimated is 0 (would divide by zero).
 *   - @p actual is &lt;= 0 (provider reported unknown / missing;
 *     xAiUsage uses -1 as the sentinel for "unknown").
 *
 * On acceptance, @c samples increments (saturating at SIZE_MAX).
 *
 * Thread model: caller must serialise updates. Session only calls
 * this from the on_done forwarder, which runs on the event-loop
 * thread, so single-session use is naturally serialised.
 *
 * @param c          calibrator to update in place.
 * @param estimated  the pre-submit static estimate (after any
 *                   prior calibration, i.e. what the gate actually
 *                   compared against @c max_tokens).
 * @param actual     provider-reported @c prompt_tokens from
 *                   @ref xAiUsage for this round. Pass -1 (or any
 *                   non-positive value) to signal "unknown".
 */
void ai_budget_calibrator_update(xAiBudgetCalibrator *c, size_t estimated,
                                 int actual);
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
 * @brief Calibrated variant of @ref ai_budget_estimate_tokens.
 *
 * Computes the raw static estimate and multiplies by the supplied
 * @p factor, rounding to the nearest integer. A @p factor of 1.0
 * is a bit-identical passthrough to the uncalibrated function.
 *
 * Returns 0 for empty inputs. @p factor is expected to be within
 * the clamp range defined by @ref XAI_BUDGET_CALIBRATION_MIN_FACTOR
 * and @ref XAI_BUDGET_CALIBRATION_MAX_FACTOR, but the function does
 * not re-clamp: callers must feed a value produced by the
 * calibrator (or a hard-coded 1.0 when they explicitly want the
 * uncalibrated answer).
 *
 * Split from the plain estimator so tests can reproduce the exact
 * arithmetic and so the calibration path stays opt-in per call
 * site — the uncalibrated estimator remains a pure, history-only
 * function with no session coupling.
 */
size_t ai_budget_estimate_tokens_calibrated(const struct xAiSessionMsg_ *msgs,
                                            size_t n, double factor);

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

/**
 * @brief Compute the fraction of token cost attributable to tool-use
 *        / tool-result entries in the given slice.
 *
 * Returns a value in [0.0, 1.0] where 1.0 means every token in the
 * slice comes from ToolUse or ToolResult entries, and 0.0 means none
 * of them do (pure text / thinking conversation). Used by the Auto
 * budget policy to decide whether SummarizeOldest (good for text) or
 * TruncateOldest (safer for structured tool data) is the better
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
double ai_budget_tool_ratio(const struct xAiSessionMsg_ *msgs, size_t n);

/**
 * @brief Threshold above which the Auto policy prefers TruncateOldest
 *        over SummarizeOldest.
 *
 * When tool entries dominate the token budget (≥ 40 %), summarising
 * is counter-productive: the LLM cannot meaningfully compress
 * structured JSON tool arguments / results, and a bad summary may
 * drop critical IDs or parameters. Truncating is safer and faster
 * in that regime.
 *
 * Below this threshold, the conversation is predominantly text and
 * SummarizeOldest has a good chance of preserving the gist.
 */
#define XAI_BUDGET_AUTO_TOOL_RATIO_THRESHOLD 0.4

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XAI_BUDGET_PRIVATE_H */
