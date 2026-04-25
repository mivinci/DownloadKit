/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_test.cpp - Whitebox smoke tests for xAiQuery_ state machine.
 *
 * Rationale:
 *   The end-to-end behaviour of Query (provider round, tool loop,
 *   usage accumulation, cancellation) is already covered by
 *   session_test.cpp through the public xAiSession surface. These
 *   tests zero in on the Query's internal state machine in
 *   isolation, so a future refactor that shuffles only the Query
 *   layer breaks here first rather than in the larger session
 *   suite.
 *
 *   Two questions we answer:
 *
 *     1. ai_query_arm() turns an idle Query into a submitted-ready
 *        one with the right invariants (usage reset to all-(-1),
 *        running flag set, session back-pointer wired).
 *     2. ai_query_reset_round() / ai_query_dispose() leave the
 *        Query in a sane, leak-free shape.
 *
 *   We deliberately do NOT reach through ai_query_submit here —
 *   that path would require the full provider/agent/event-loop
 *   scaffolding, which is already exercised verbatim by
 *   session_test. Duplicating it would test the scaffolding, not
 *   the Query.
 */

extern "C" {
#include "query_private.h"
#include "session_private.h"

#include <xbase/error.h>
}

#include <cstring>

#include <gtest/gtest.h>

namespace {

/* Zero-shape a Session just enough that ai_query_arm can wire the
 * back-pointer. The Query does not dereference any Session field
 * during arm / reset / dispose; it only needs a non-NULL address to
 * record as q->session. */
struct FakeSession {
  xAiSession_ s;

  FakeSession() {
    std::memset(&s, 0, sizeof(s));
  }
  xAiSession_ *get() { return &s; }
};

}  // namespace

/* ── ai_query_arm ─────────────────────────────────────────────────── */

TEST(QueryState, ArmWiresSessionBackpointer) {
  FakeSession fs;
  xAiQuery_   q;
  std::memset(&q, 0, sizeof(q));

  ai_query_arm(&q, fs.get());

  EXPECT_EQ(q.session, fs.get());
  EXPECT_EQ(q.running, 1);
  EXPECT_EQ(q.cancelled, 0);
  EXPECT_EQ(q.turn, 0);
}

TEST(QueryState, ArmResetsUsageToSentinels) {
  FakeSession fs;
  xAiQuery_   q;
  std::memset(&q, 0, sizeof(q));
  /* Prime the usage with stale non-sentinel data; arm must wipe it. */
  q.saw_usage                = 1;
  q.usage.prompt_tokens      = 42;
  q.usage.completion_tokens  = 99;
  q.usage.total_tokens       = 141;

  ai_query_arm(&q, fs.get());

  EXPECT_EQ(q.saw_usage, 0);
  EXPECT_EQ(q.usage.prompt_tokens, -1);
  EXPECT_EQ(q.usage.completion_tokens, -1);
  EXPECT_EQ(q.usage.total_tokens, -1);
}

/* ── ai_query_cancel_mark ─────────────────────────────────────────── */

TEST(QueryState, CancelMarkFlipsFlagWithoutTouchingRunning) {
  FakeSession fs;
  xAiQuery_   q;
  std::memset(&q, 0, sizeof(q));
  ai_query_arm(&q, fs.get());

  ai_query_cancel_mark(&q);

  EXPECT_EQ(q.cancelled, 1);
  /* running stays 1 — finish_run is what clears it. cancel_mark
   * only raises the flag so the next callback bail-out can observe
   * it. */
  EXPECT_EQ(q.running, 1);
}

/* ── ai_query_reset_round ─────────────────────────────────────────── */

TEST(QueryState, ResetRoundClearsRunFlagsAndUsage) {
  FakeSession fs;
  xAiQuery_   q;
  std::memset(&q, 0, sizeof(q));
  ai_query_arm(&q, fs.get());
  q.turn                    = 3;
  q.saw_usage               = 1;
  q.usage.prompt_tokens     = 10;
  q.usage.completion_tokens = 20;
  q.cancelled               = 1;

  ai_query_reset_round(&q);

  EXPECT_EQ(q.running, 0);
  EXPECT_EQ(q.cancelled, 0);
  EXPECT_EQ(q.turn, 0);
  EXPECT_EQ(q.saw_usage, 0);
  EXPECT_EQ(q.usage.prompt_tokens, -1);
  EXPECT_EQ(q.usage.completion_tokens, -1);
  EXPECT_EQ(q.usage.total_tokens, -1);
}

/* ── ai_query_dispose ─────────────────────────────────────────────── */

TEST(QueryState, DisposeOnZeroQueryIsSafe) {
  /* An idle Query that never saw arm() must still tolerate dispose
   * — this matches session.c's unconditional call in the destroy
   * path. No assertions beyond "doesn't crash / no UB under ASan". */
  xAiQuery_ q;
  std::memset(&q, 0, sizeof(q));
  ai_query_dispose(&q);
  SUCCEED();
}

TEST(QueryState, DisposeAfterArmIsSafe) {
  FakeSession fs;
  xAiQuery_   q;
  std::memset(&q, 0, sizeof(q));
  ai_query_arm(&q, fs.get());
  /* No provider round ran, so no heap buffers were allocated; this
   * still exercises the dispose path for the common
   * "create-then-destroy without ever sending input" shape. */
  ai_query_dispose(&q);
  SUCCEED();
}
