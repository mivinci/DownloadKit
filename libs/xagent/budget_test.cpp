/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * budget_test.cpp - Unit tests for budget_private.h helpers.
 *
 * These functions are pure: no allocations, no state, no IO. Tests
 * construct small xAgentSessionMsg_ fixtures on the stack, feed them
 * through the helpers, and assert on the numeric return values.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xagent/message.h> /* xAgentRole_* */
}

#include "budget_private.h"
#include "turn_private.h"

#include <cstddef>
#include <cstring>

/* ── Fixture helpers ──────────────────────────────────────────────────
 *
 * These builders return a zeroed xAgentSessionMsg_ with just the fields
 * relevant to a given kind populated. They take `char *` instead of
 * `const char *` where the real struct does, because turn_private.h
 * models owned storage that way and we do not want to sprinkle
 * const_cast through every test case. Ownership is moot here — the
 * helpers under test never free or mutate the fixtures. */

namespace {

xAgentSessionMsg_ MakeText(xAgentRole role, const char *s) {
  xAgentSessionMsg_ m{};
  m.role     = role;
  m.kind     = xAgentSessionEntry_Text;
  m.text     = const_cast<char *>(s);
  m.text_len = s ? std::strlen(s) : 0;
  return m;
}

xAgentSessionMsg_ MakeThinking(const char *s) {
  xAgentSessionMsg_ m{};
  m.role     = xAgentRole_Assistant;
  m.kind     = xAgentSessionEntry_Thinking;
  m.text     = const_cast<char *>(s);
  m.text_len = s ? std::strlen(s) : 0;
  return m;
}

xAgentSessionMsg_ MakeToolUse(const char *id, const char *name, const char *args) {
  xAgentSessionMsg_ m{};
  m.role          = xAgentRole_Assistant;
  m.kind          = xAgentSessionEntry_ToolUse;
  m.tool_use_id   = const_cast<char *>(id);
  m.tool_use_name = const_cast<char *>(name);
  m.tool_use_args = const_cast<char *>(args);
  return m;
}

xAgentSessionMsg_ MakeToolResult(const char *id, const char *output,
                              int is_error) {
  xAgentSessionMsg_ m{};
  m.role                   = xAgentRole_Tool;
  m.kind                   = xAgentSessionEntry_ToolResult;
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
  xAgentSessionMsg_ m = MakeText(xAgentRole_User, "abcdefgh");
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            8u / XAGENT_BUDGET_BYTES_PER_TOKEN + XAGENT_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ThinkingCountsSameAsText) {
  xAgentSessionMsg_ a = MakeText(xAgentRole_Assistant, "abcdabcd"); /* 8 */
  xAgentSessionMsg_ b = MakeThinking("abcdabcd");                /* 8 */
  EXPECT_EQ(ai_budget_estimate_tokens(&a, 1), ai_budget_estimate_tokens(&b, 1));
}

TEST(XaiBudgetEstimate, ToolUseSumsNameAndArgsIgnoresId) {
  /* Only name ("calc" = 4) + args ("{}" = 2) count, id does not. */
  xAgentSessionMsg_ m = MakeToolUse("toolu_ignored", "calc", "{}");
  /* payload = 4 + 2 = 6; 6/4 = 1; + envelope 8 = 9. */
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            6u / XAGENT_BUDGET_BYTES_PER_TOKEN + XAGENT_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ToolUseWithNullFieldsIsSafe) {
  xAgentSessionMsg_ m{};
  m.role = xAgentRole_Assistant;
  m.kind = xAgentSessionEntry_ToolUse;
  /* All three string fields left NULL. Must not crash; payload 0. */
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1), XAGENT_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, ToolResultUsesOutputLen) {
  xAgentSessionMsg_ m = MakeToolResult("id", "12345678", /*is_error=*/0);
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1),
            8u / XAGENT_BUDGET_BYTES_PER_TOKEN + XAGENT_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, MixedSliceSumsPerEntry) {
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "hello"),         /* 5 */
    MakeText(xAgentRole_Assistant, "hi!"),      /* 3 */
    MakeToolUse("i", "calc", "{\"x\":1}"),   /* 4 + 7 = 11 */
    MakeToolResult("i", "2", /*err=*/0),     /* 1 */
    MakeThinking("reasoning steps aplenty"), /* 22 */
  };
  const size_t n = sizeof(seq) / sizeof(seq[0]);
  /* payload = 5 + 3 + 11 + 1 + 22 = 42; 42/4 = 10. + 5*8 = 50. */
  EXPECT_EQ(ai_budget_estimate_tokens(seq, n),
            42u / XAGENT_BUDGET_BYTES_PER_TOKEN + n * XAGENT_BUDGET_PER_MSG_TOKENS);
}

TEST(XaiBudgetEstimate, EmptyTextStillPaysEnvelope) {
  /* Even zero-byte payloads cost one envelope. */
  xAgentSessionMsg_ m = MakeText(xAgentRole_User, "");
  EXPECT_EQ(ai_budget_estimate_tokens(&m, 1), XAGENT_BUDGET_PER_MSG_TOKENS);
}

/* ── ai_budget_find_user_turn ───────────────────────────────────── */

TEST(XaiBudgetFindNth, EmptyReturnsSentinel) {
  EXPECT_EQ(ai_budget_find_user_turn(nullptr, 0, 0),
            XAGENT_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, NoUserEntries) {
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_Assistant, "reply"),
    MakeThinking("thinking"),
  };
  EXPECT_EQ(ai_budget_find_user_turn(seq, 2, 0), XAGENT_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, FirstUserEntry) {
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),      /* idx 0 */
    MakeText(xAgentRole_Assistant, "a1"), /* idx 1 */
    MakeText(xAgentRole_User, "q2"),      /* idx 2 */
  };
  EXPECT_EQ(ai_budget_find_user_turn(seq, 3, 0), 0u);
  EXPECT_EQ(ai_budget_find_user_turn(seq, 3, 1), 2u);
}

TEST(XaiBudgetFindNth, OutOfRangeReturnsSentinel) {
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "only"),
  };
  EXPECT_EQ(ai_budget_find_user_turn(seq, 1, 1), XAGENT_BUDGET_NO_SUCH_TURN);
  EXPECT_EQ(ai_budget_find_user_turn(seq, 1, 42), XAGENT_BUDGET_NO_SUCH_TURN);
}

TEST(XaiBudgetFindNth, SkipsAssistantAndTool) {
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_Assistant, "prelude"),
    MakeToolUse("i", "calc", "{}"),
    MakeToolResult("i", "ok", 0),
    MakeText(xAgentRole_User, "real-first"), /* idx 3 */
    MakeText(xAgentRole_Assistant, "bye"),
    MakeText(xAgentRole_User, "real-second"), /* idx 5 */
  };
  EXPECT_EQ(ai_budget_find_user_turn(seq, 6, 0), 3u);
  EXPECT_EQ(ai_budget_find_user_turn(seq, 6, 1), 5u);
}

/* ── ai_budget_recent_band_start ────────────────────────────────────────── */

TEST(XaiBudgetRecentBandStart, EmptyHistory) {
  EXPECT_EQ(ai_budget_recent_band_start(nullptr, 0, 0), 0u);
  EXPECT_EQ(ai_budget_recent_band_start(nullptr, 0, 5), 0u);
}

TEST(XaiBudgetRecentBandStart, NoUserTurnsKeepEverything) {
  /* Degenerate fixture (history built outside the normal
   * append path) — contract says: nothing to anchor a boundary,
   * play it safe and return 0. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_Assistant, "orphan"),
  };
  EXPECT_EQ(ai_budget_recent_band_start(seq, 1, 0), 0u);
  EXPECT_EQ(ai_budget_recent_band_start(seq, 1, 3), 0u);
}

TEST(XaiBudgetRecentBandStart, KeepZeroKeepsOnlyLastUserTurn) {
  /* keep_recent_turns == 0 collapses to "start keeping at the last
   * User-role entry". Everything before that index is droppable. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),      /* 0 */
    MakeText(xAgentRole_Assistant, "a1"), /* 1 */
    MakeText(xAgentRole_User, "q2"),      /* 2 */
    MakeText(xAgentRole_Assistant, "a2"), /* 3 */
    MakeText(xAgentRole_User, "q3"),      /* 4 ← earliest keep */
  };
  EXPECT_EQ(ai_budget_recent_band_start(seq, 5, 0), 4u);
}

TEST(XaiBudgetRecentBandStart, FloorExceedsHistoryKeepsEverything) {
  /* 2 user turns total, caller demands a floor of 5 → we return 0.
   * We would rather keep too much than drop below the floor. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),
    MakeText(xAgentRole_Assistant, "a1"),
    MakeText(xAgentRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_recent_band_start(seq, 3, 5), 0u);
}

TEST(XaiBudgetRecentBandStart, FloorEqualsTotalKeepsEverything) {
  /* Exactly at the floor: still return 0, because dropping even
   * one turn would put us strictly below. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),
    MakeText(xAgentRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_recent_band_start(seq, 2, 2), 0u);
}

TEST(XaiBudgetRecentBandStart, KeepsLastTwoTurnsWithToolChatter) {
  /* Five user turns interleaved with assistant / tool entries.
   * keep_recent_turns = 2 → start keeping at the 4th user turn
   * (user_count - keep_recent_turns = 5 - 2 = 3 → the k=3-th User
   * entry, 0-indexed). */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "u1"),      /* 0 */
    MakeText(xAgentRole_Assistant, "a1"), /* 1 */
    MakeText(xAgentRole_User, "u2"),      /* 2 */
    MakeToolUse("i2", "calc", "{}"),   /* 3 */
    MakeToolResult("i2", "4", 0),      /* 4 */
    MakeText(xAgentRole_Assistant, "a2"), /* 5 */
    MakeText(xAgentRole_User, "u3"),      /* 6 */
    MakeText(xAgentRole_Assistant, "a3"), /* 7 */
    MakeText(xAgentRole_User, "u4"),      /* 8 ← earliest keep */
    MakeText(xAgentRole_Assistant, "a4"), /* 9 */
    MakeText(xAgentRole_User, "u5"),      /* 10 */
  };
  EXPECT_EQ(ai_budget_recent_band_start(seq, 11, 2), 8u);
}

TEST(XaiBudgetRecentBandStart, BoundaryIsAlwaysAUserRoleIndex) {
  /* Regression barrier for the core invariant: wherever the function
   * points, that slot must carry a User role (or be 0 = sentinel
   * "keep everything"). This test iterates a variety of shapes and
   * asserts the boundary invariant directly. */
  struct Case {
    size_t keep_recent;
    size_t expect_idx;
  };
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "u1"),      MakeToolUse("a", "t", "{}"),
    MakeToolResult("a", "r", 0),       MakeText(xAgentRole_User, "u2"),
    MakeText(xAgentRole_Assistant, "a2"), MakeText(xAgentRole_User, "u3"),
  };
  const size_t n = sizeof(seq) / sizeof(seq[0]);

  const Case cases[] = {
    {0, 5}, /* last user turn */
    {1, 5},
    {2, 3},
    {3, 0}, /* floor == total → keep all */
  };
  for (const auto &c : cases) {
    size_t idx = ai_budget_recent_band_start(seq, n, c.keep_recent);
    EXPECT_EQ(idx, c.expect_idx) << "keep_recent=" << c.keep_recent;
    if (idx != 0) {
      EXPECT_EQ(seq[idx].role, xAgentRole_User)
        << "boundary at idx " << idx
        << " must be a User-role entry (keep_recent=" << c.keep_recent << ")";
    }
  }
}

/* ── ai_budget_head_band_end ─────────────────────────────────────────── */

TEST(XaiBudgetHeadBandEnd, EmptyHistory) {
  EXPECT_EQ(ai_budget_head_band_end(nullptr, 0, 0), 0u);
  EXPECT_EQ(ai_budget_head_band_end(nullptr, 0, 5), 0u);
}

TEST(XaiBudgetHeadBandEnd, NoUserTurnsKeepsAll) {
  /* No User entries → nothing anchors a boundary → keep everything. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_Assistant, "orphan"),
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 1, 3), 1u);
}

TEST(XaiBudgetHeadBandEnd, KeepZeroMeansNoPrefix) {
  /* keep_prefix_turns == 0: no mandatory prefix → head band is empty.
   * Return 0: everything can be dropped from the tail side. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),      /* 0 */
    MakeText(xAgentRole_Assistant, "a1"), /* 1 */
    MakeText(xAgentRole_User, "q2"),      /* 2 */
    MakeText(xAgentRole_Assistant, "a2"), /* 3 */
    MakeText(xAgentRole_User, "q3"),      /* 4 */
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 5, 0), 0u);
}

TEST(XaiBudgetHeadBandEnd, FloorExceedsHistoryKeepsAll) {
  /* 2 user turns total, caller demands a floor of 5 → keep all. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),
    MakeText(xAgentRole_Assistant, "a1"),
    MakeText(xAgentRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 3, 5), 3u);
}

TEST(XaiBudgetHeadBandEnd, FloorEqualsTotalKeepsAll) {
  /* Exactly at the floor: keep everything. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),
    MakeText(xAgentRole_User, "q2"),
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 2, 2), 2u);
}

TEST(XaiBudgetHeadBandEnd, KeepsFirstTwoTurnsWithToolChatter) {
  /* Five user turns. keep_prefix_turns = 2 → keep the first 2 User
   * turn groups. The boundary is the start of the 3rd User entry. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "u1"),      /* 0 */
    MakeText(xAgentRole_Assistant, "a1"), /* 1 */
    MakeText(xAgentRole_User, "u2"),      /* 2 */
    MakeToolUse("i2", "calc", "{}"),   /* 3 */
    MakeToolResult("i2", "4", 0),      /* 4 */
    MakeText(xAgentRole_Assistant, "a2"), /* 5 */
    MakeText(xAgentRole_User, "u3"),      /* 6 ← boundary */
    MakeText(xAgentRole_Assistant, "a3"), /* 7 */
    MakeText(xAgentRole_User, "u4"),      /* 8 */
    MakeText(xAgentRole_Assistant, "a4"), /* 9 */
    MakeText(xAgentRole_User, "u5"),      /* 10 */
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 11, 2), 6u);
}

TEST(XaiBudgetHeadBandEnd, BoundaryLandsOnUserRole) {
  /* The boundary index must always be a User-role entry (or n). */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "u1"),      MakeToolUse("a", "t", "{}"),
    MakeToolResult("a", "r", 0),       MakeText(xAgentRole_User, "u2"),
    MakeText(xAgentRole_Assistant, "a2"), MakeText(xAgentRole_User, "u3"),
  };
  const size_t n = sizeof(seq) / sizeof(seq[0]);

  /* keep_prefix_turns = 1 → keep first turn group (u1 + tool chatter).
   * Boundary = start of 2nd User entry (u2 at index 3). */
  size_t idx = ai_budget_head_band_end(seq, n, 1);
  EXPECT_EQ(idx, 3u);
  EXPECT_EQ(seq[idx].role, xAgentRole_User);
}

TEST(XaiBudgetHeadBandEnd, SingleUserTurnKeepsAll) {
  /* Only one User entry → no room to trim. */
  xAgentSessionMsg_ seq[] = {
    MakeText(xAgentRole_User, "q1"),
    MakeText(xAgentRole_Assistant, "a1"),
  };
  EXPECT_EQ(ai_budget_head_band_end(seq, 2, 1), 2u);
  EXPECT_EQ(ai_budget_head_band_end(seq, 2, 0), 0u); /* no prefix required */
}
