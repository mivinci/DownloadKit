/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_test.cpp - Unit tests for budget_private.h helpers.
 *
 * These functions are pure: no allocations, no state, no IO. Tests
 * construct small xAiSessionMsg_ fixtures on the stack, feed them
 * through the helpers, and assert on the numeric return values.
 * Nothing in budget.c has been wired into the session lifecycle
 * yet — c3 does that — so these tests are the only coverage for
 * c2 and MUST exercise every branch.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xai/message.h> /* xAiRole_* */
}

#include "budget_private.h"
#include "turn_private.h"

#include <cstddef>
#include <cstring>

/* ── Fixture helpers ──────────────────────────────────────────────────
 *
 * These builders return a zeroed xAiSessionMsg_ with just the fields
 * relevant to a given kind populated. They take `char *` instead of
 * `const char *` where the real struct does, because turn_private.h
 * models owned storage that way and we do not want to sprinkle
 * const_cast through every test case. Ownership is moot here — the
 * helpers under test never free or mutate the fixtures. */

namespace {

xAiSessionMsg_ MakeText(xAiRole role, const char *s) {
  xAiSessionMsg_ m{};
  m.role     = role;
  m.kind     = xAiSessionEntry_Text;
  m.text     = const_cast<char *>(s);
  m.text_len = s ? std::strlen(s) : 0;
  return m;
}

xAiSessionMsg_ MakeThinking(const char *s) {
  xAiSessionMsg_ m{};
  m.role     = xAiRole_Assistant;
  m.kind     = xAiSessionEntry_Thinking;
  m.text     = const_cast<char *>(s);
  m.text_len = s ? std::strlen(s) : 0;
  return m;
}

xAiSessionMsg_ MakeToolUse(const char *id, const char *name, const char *args) {
  xAiSessionMsg_ m{};
  m.role          = xAiRole_Assistant;
  m.kind          = xAiSessionEntry_ToolUse;
  m.tool_use_id   = const_cast<char *>(id);
  m.tool_use_name = const_cast<char *>(name);
  m.tool_use_args = const_cast<char *>(args);
  return m;
}

xAiSessionMsg_ MakeToolResult(const char *id, const char *output,
                              int is_error) {
  xAiSessionMsg_ m{};
  m.role                   = xAiRole_Tool;
  m.kind                   = xAiSessionEntry_ToolResult;
  m.tool_result_id         = const_cast<char *>(id);
  m.tool_result_output     = const_cast<char *>(output);
  m.tool_result_output_len = output ? std::strlen(output) : 0;
  m.tool_result_is_error   = is_error;
  return m;
}

} /* namespace */

/* ── ai_budget_estimate_tokens ──────────────────────────────────────── */

TEST(XaiBudgetEstimate, EmptyIsZero) {
  EXPECT_EQ(ai_budget_estimate_tokens(nullptr, 0), 0u);
}

TEST(XaiBudgetEstimate, NullSliceWithZeroCount) {
  /* n == 0 short-circuits before dereferencing msgs. */
  EXPECT_EQ(ai_budget_estimate_tokens(nullptr, 0), 0u);
}

TEST(XaiBudgetEstimate, SingleTextUsesBytesDivFourPlusEnvelope) {
  /* 8 bytes of payload + one envelope of 8 tokens = 8/4 + 8 = 10. */
  xAiSessionMsg_ m = MakeText(xAiRole_User, "abcdefgh");
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            8u / XAI_BUDGET_BYTES_PER_TOKEN + XAI_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ThinkingCountsSameAsText) {
  xAiSessionMsg_ a = MakeText(xAiRole_Assistant, "abcdabcd"); /* 8 */
  xAiSessionMsg_ b = MakeThinking("abcdabcd");                /* 8 */
  EXPECT_EQ(ai_budget_estimate_tokens(&a, 1), ai_budget_estimate_tokens(&b, 1));
}

TEST(XaiBudgetEstimate, ToolUseSumsNameAndArgsIgnoresId) {
  /* Only name ("calc" = 4) + args ("{}" = 2) count, id does not. */
  xAiSessionMsg_ m = MakeToolUse("toolu_ignored", "calc", "{}");
  /* payload = 4 + 2 = 6; 6/4 = 1; + envelope 8 = 9. */
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            6u / XAI_BUDGET_BYTES_PER_TOKEN + XAI_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ToolUseWithNullFieldsIsSafe) {
  xAiSessionMsg_ m{};
  m.role = xAiRole_Assistant;
  m.kind = xAiSessionEntry_ToolUse;
  /* All three string fields left NULL. Must not crash; payload 0. */
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1), XAI_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ToolResultUsesOutputLen) {
  xAiSessionMsg_ m = MakeToolResult("id", "12345678", /*is_error=*/0);
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            8u / XAI_BUDGET_BYTES_PER_TOKEN + XAI_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, MixedSliceSumsPerEntry) {
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "hello"),         /* 5 */
    MakeText(xAiRole_Assistant, "hi!"),      /* 3 */
    MakeToolUse("i", "calc", "{\"x\":1}"),   /* 4 + 7 = 11 */
    MakeToolResult("i", "2", /*err=*/0),     /* 1 */
    MakeThinking("reasoning steps aplenty"), /* 22 */
  };
  const size_t n = sizeof(seq) / sizeof(seq[0]);
  /* payload = 5 + 3 + 11 + 1 + 22 = 42; 42/4 = 10. + 5*8 = 50. */
  EXPECT_EQ(ai_budget_estimate_tokens(seq, n),
            42u / XAI_BUDGET_BYTES_PER_TOKEN + n * XAI_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, EmptyTextStillPaysEnvelope) {
  /* Even zero-byte payloads cost one envelope. */
  xAiSessionMsg_ m = MakeText(xAiRole_User, "");
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1), XAI_BUDGET_PER_MSG_TOKENS);
}

/* ── ai_budget_find_nth_user_turn ───────────────────────────────────── */

TEST(XaiBudgetFindNth, EmptyReturnsSentinel) {
  EXPECT_EQ(ai_budget_find_nth_user_turn(nullptr, 0, 0),
            XAI_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, NoUserEntries) {
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_Assistant, "reply"),
    MakeThinking("thinking"),
  };
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 2, 0), XAI_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, FirstUserEntry) {
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "q1"),      /* idx 0 */
    MakeText(xAiRole_Assistant, "a1"), /* idx 1 */
    MakeText(xAiRole_User, "q2"),      /* idx 2 */
  };
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 3, 0), 0u);
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 3, 1), 2u);
}

TEST(XaiBudgetFindNth, OutOfRangeReturnsSentinel) {
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "only"),
  };
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 1, 1), XAI_BUDGET_NO_SUCH_TURN);
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 1, 42), XAI_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, SkipsAssistantAndTool) {
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_Assistant, "prelude"),
    MakeToolUse("i", "calc", "{}"),
    MakeToolResult("i", "ok", 0),
    MakeText(xAiRole_User, "real-first"), /* idx 3 */
    MakeText(xAiRole_Assistant, "bye"),
    MakeText(xAiRole_User, "real-second"), /* idx 5 */
  };
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 6, 0), 3u);
  EXPECT_EQ(ai_budget_find_nth_user_turn(seq, 6, 1), 5u);
}

/* ── ai_budget_earliest_keep ────────────────────────────────────────── */

TEST(XaiBudgetEarliestKeep, EmptyHistory) {
  EXPECT_EQ(ai_budget_earliest_keep(nullptr, 0, 0), 0u);
  EXPECT_EQ(ai_budget_earliest_keep(nullptr, 0, 5), 0u);
}

TEST(XaiBudgetEarliestKeep, NoUserTurnsKeepEverything) {
  /* Degenerate fixture (history built outside the normal
   * append path) — contract says: nothing to anchor a boundary,
   * play it safe and return 0. */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_Assistant, "orphan"),
  };
  EXPECT_EQ(ai_budget_earliest_keep(seq, 1, 0), 0u);
  EXPECT_EQ(ai_budget_earliest_keep(seq, 1, 3), 0u);
}

TEST(XaiBudgetEarliestKeep, KeepZeroKeepsOnlyLastUserTurn) {
  /* keep_recent_turns == 0 collapses to "start keeping at the last
   * User-role entry". Everything before that index is droppable. */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "q1"),      /* 0 */
    MakeText(xAiRole_Assistant, "a1"), /* 1 */
    MakeText(xAiRole_User, "q2"),      /* 2 */
    MakeText(xAiRole_Assistant, "a2"), /* 3 */
    MakeText(xAiRole_User, "q3"),      /* 4 ← earliest keep */
  };
  EXPECT_EQ(ai_budget_earliest_keep(seq, 5, 0), 4u);
}

TEST(XaiBudgetEarliestKeep, FloorExceedsHistoryKeepsEverything) {
  /* 2 user turns total, caller demands a floor of 5 → we return 0.
   * We would rather keep too much than drop below the floor. */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "q1"),
    MakeText(xAiRole_Assistant, "a1"),
    MakeText(xAiRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_earliest_keep(seq, 3, 5), 0u);
}

TEST(XaiBudgetEarliestKeep, FloorEqualsTotalKeepsEverything) {
  /* Exactly at the floor: still return 0, because dropping even
   * one turn would put us strictly below. */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "q1"),
    MakeText(xAiRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_earliest_keep(seq, 2, 2), 0u);
}

TEST(XaiBudgetEarliestKeep, KeepsLastTwoTurnsWithToolChatter) {
  /* Five user turns interleaved with assistant / tool entries.
   * keep_recent_turns = 2 → start keeping at the 4th user turn
   * (user_count - keep_recent_turns = 5 - 2 = 3 → the k=3-th User
   * entry, 0-indexed). */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "u1"),      /* 0 */
    MakeText(xAiRole_Assistant, "a1"), /* 1 */
    MakeText(xAiRole_User, "u2"),      /* 2 */
    MakeToolUse("i2", "calc", "{}"),   /* 3 */
    MakeToolResult("i2", "4", 0),      /* 4 */
    MakeText(xAiRole_Assistant, "a2"), /* 5 */
    MakeText(xAiRole_User, "u3"),      /* 6 */
    MakeText(xAiRole_Assistant, "a3"), /* 7 */
    MakeText(xAiRole_User, "u4"),      /* 8 ← earliest keep */
    MakeText(xAiRole_Assistant, "a4"), /* 9 */
    MakeText(xAiRole_User, "u5"),      /* 10 */
  };
  EXPECT_EQ(ai_budget_earliest_keep(seq, 11, 2), 8u);
}

TEST(XaiBudgetEarliestKeep, BoundaryIsAlwaysAUserRoleIndex) {
  /* Regression barrier for the core invariant: wherever the function
   * points, that slot must carry a User role (or be 0 = sentinel
   * "keep everything"). This test iterates a variety of shapes and
   * asserts the boundary invariant directly. */
  struct Case {
    size_t keep_recent;
    size_t expect_idx;
  };
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "u1"),      MakeToolUse("a", "t", "{}"),
    MakeToolResult("a", "r", 0),       MakeText(xAiRole_User, "u2"),
    MakeText(xAiRole_Assistant, "a2"), MakeText(xAiRole_User, "u3"),
  };
  const size_t n = sizeof(seq) / sizeof(seq[0]);

  const Case cases[] = {
    {0, 5}, /* last user turn */
    {1, 5},
    {2, 3},
    {3, 0}, /* floor == total → keep all */
  };
  for (const auto &c : cases) {
    size_t idx = ai_budget_earliest_keep(seq, n, c.keep_recent);
    EXPECT_EQ(idx, c.expect_idx) << "keep_recent=" << c.keep_recent;
    if (idx != 0) {
      EXPECT_EQ(seq[idx].role, xAiRole_User)
        << "boundary at idx " << idx
        << " must be a User-role entry (keep_recent=" << c.keep_recent << ")";
    }
  }
}

/* ── Calibrated estimator & calibrator state machine (c4) ──────── */

TEST(XaiBudgetCalibratedEstimate, FactorOneIsIdentity) {
  /* factor = 1.0 must be a bit-identical passthrough to the raw
   * estimator; this anchors the backward-compatibility promise for
   * freshly created sessions (which ship with factor = 1.0). */
  xAiSessionMsg_ seq[] = {
    MakeText(xAiRole_User, "hello world"),
    MakeText(xAiRole_Assistant, "hi there"),
  };
  size_t raw = ai_budget_estimate_tokens(seq, 2);
  size_t cal = ai_budget_estimate_tokens_calibrated(seq, 2, 1.0);
  EXPECT_EQ(raw, cal);
}

TEST(XaiBudgetCalibratedEstimate, EmptyInputIsAlwaysZero) {
  /* Both the raw and calibrated estimators must short-circuit on
   * empty input regardless of factor. A nonzero factor would
   * otherwise amplify a nonexistent envelope tax. */
  EXPECT_EQ(ai_budget_estimate_tokens_calibrated(nullptr, 0, 1.0), 0u);
  EXPECT_EQ(ai_budget_estimate_tokens_calibrated(nullptr, 0, 2.0), 0u);
  xAiSessionMsg_ dummy{};
  EXPECT_EQ(ai_budget_estimate_tokens_calibrated(&dummy, 0, 0.5), 0u);
}

TEST(XaiBudgetCalibratedEstimate, FactorScalesAndRounds) {
  /* Pick a payload whose raw estimate is large enough that 1.5x
   * and 0.5x produce distinct integer answers:
   *   400 bytes → 400/4 = 100 payload tokens
   *              + 1 * XAI_BUDGET_PER_MSG_TOKENS (8) = 108 raw.
   *   108 * 1.5 = 162.0 → 162.
   *   108 * 0.5 =  54.0 →  54. */
  std::string big(400, 'x');
  xAiSessionMsg_ seq[] = {MakeText(xAiRole_User, big.c_str())};
  size_t raw = ai_budget_estimate_tokens(seq, 1);
  ASSERT_EQ(raw, 108u);
  EXPECT_EQ(ai_budget_estimate_tokens_calibrated(seq, 1, 1.5), 162u);
  EXPECT_EQ(ai_budget_estimate_tokens_calibrated(seq, 1, 0.5),  54u);
}

TEST(XaiBudgetCalibrator, InitStartsAtIdentity) {
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  EXPECT_DOUBLE_EQ(c.factor, 1.0);
  EXPECT_EQ(c.samples, 0u);
}

TEST(XaiBudgetCalibrator, NullIsTolerated) {
  /* Defensive: init(NULL) and update(NULL, ...) must not crash.
   * Keeps callers honest without forcing guards at every site. */
  ai_budget_calibrator_init(nullptr);
  ai_budget_calibrator_update(nullptr, 100, 90);
  SUCCEED();
}

TEST(XaiBudgetCalibrator, ZeroEstimatedIsIgnored) {
  /* Dividing by zero would poison the factor. Verify the guard
   * keeps state untouched and does NOT count this as a sample. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  ai_budget_calibrator_update(&c, 0, 100);
  EXPECT_DOUBLE_EQ(c.factor, 1.0);
  EXPECT_EQ(c.samples, 0u);
}

TEST(XaiBudgetCalibrator, UnknownActualIsIgnored) {
  /* xAiUsage uses -1 as the "unknown" sentinel. The calibrator
   * must reject any non-positive actual value without touching
   * state. Tests -1, 0 and a large negative to be thorough. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  for (int bad : {-1, 0, -9999}) {
    ai_budget_calibrator_update(&c, 100, bad);
    EXPECT_DOUBLE_EQ(c.factor, 1.0) << "bad=" << bad;
    EXPECT_EQ(c.samples, 0u) << "bad=" << bad;
  }
}

TEST(XaiBudgetCalibrator, SingleObservationMovesFactorByAlpha) {
  /* One observation with observed = 2.0 should move the factor
   * exactly ALPHA of the way there:
   *   next = (1 - ALPHA) * 1.0 + ALPHA * 2.0 = 1 + ALPHA.
   * With ALPHA = 0.25 the expected factor is 1.25, still inside
   * the clamp range so no clamp engages. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  ai_budget_calibrator_update(&c, 100, 200); /* observed = 2.0 */
  EXPECT_NEAR(c.factor, 1.0 + XAI_BUDGET_CALIBRATION_ALPHA, 1e-9);
  EXPECT_EQ(c.samples, 1u);
}

TEST(XaiBudgetCalibrator, RepeatedObservationsConverge) {
  /* Feed a constant observed = 1.5 many times. The factor should
   * converge monotonically toward 1.5 (still inside the clamp).
   * After N steps: factor_N = 1.5 - (1 - ALPHA)^N * (1.5 - 1.0).
   * After 10 steps with ALPHA = 0.25 that is ~1.4437; after 20
   * it is ~1.4968. We assert the endpoint is within 0.01 of 1.5. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  for (int i = 0; i < 30; i++) {
    ai_budget_calibrator_update(&c, 100, 150);
  }
  EXPECT_NEAR(c.factor, 1.5, 0.01);
  EXPECT_EQ(c.samples, 30u);
}

TEST(XaiBudgetCalibrator, ClampsAtUpperBound) {
  /* Hammer the calibrator with massive observed ratios (10x) and
   * verify it never exceeds MAX_FACTOR. Chooses a value that
   * would converge to 10.0 without the clamp; with it the factor
   * saturates at the configured ceiling. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  for (int i = 0; i < 50; i++) {
    ai_budget_calibrator_update(&c, 100, 1000);
  }
  EXPECT_DOUBLE_EQ(c.factor, XAI_BUDGET_CALIBRATION_MAX_FACTOR);
}

TEST(XaiBudgetCalibrator, ClampsAtLowerBound) {
  /* Symmetric: observed = 0.1 would drive the factor toward 0.1
   * without the clamp. Verify it bottoms out at MIN_FACTOR. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  for (int i = 0; i < 50; i++) {
    ai_budget_calibrator_update(&c, 100, 10);
  }
  EXPECT_DOUBLE_EQ(c.factor, XAI_BUDGET_CALIBRATION_MIN_FACTOR);
}

TEST(XaiBudgetCalibrator, ReinitResetsState) {
  /* Safety: re-running init on a populated calibrator must fully
   * reset both factor and sample count. Tests the "discards prior
   * state" clause in the header doc. */
  xAiBudgetCalibrator c{};
  ai_budget_calibrator_init(&c);
  ai_budget_calibrator_update(&c, 100, 200);
  ai_budget_calibrator_update(&c, 100, 200);
  EXPECT_GT(c.factor, 1.0);
  EXPECT_GT(c.samples, 0u);

  ai_budget_calibrator_init(&c);
  EXPECT_DOUBLE_EQ(c.factor, 1.0);
  EXPECT_EQ(c.samples, 0u);
}
