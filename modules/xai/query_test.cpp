/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_test.cpp - Whitebox tests for the xAiQuery public API.
 *
 * Rationale:
 *   End-to-end Query behaviour (provider round, tool loop, usage
 *   accumulation, cancellation, Create/Run/Destroy lifecycle) is
 *   exercised through the public xAiSession surface in
 *   session_test.cpp. These tests only cover what that suite can't
 *   reach trivially: the NULL-safety and out-parameter contracts
 *   of the standalone xAiQuery* accessors.
 *
 *   Anything that needs a real Agent + provider stack is tested
 *   via xAiSessionInput in session_test.cpp; duplicating that
 *   setup here would test the scaffolding, not the Query.
 */

extern "C" {
#include <xai/query.h>
#include <xai/session.h>
}

#include <gtest/gtest.h>

/* ── Public API NULL / out-param contracts ─────────────────────── */

TEST(QueryPublicApi, NullHandleIsSafe) {
  EXPECT_EQ(xAiSessionQuery(nullptr), nullptr);
  EXPECT_EQ(xAiQuerySession(nullptr), nullptr);
  EXPECT_EQ(xAiQueryIsRunning(nullptr), 0);
  EXPECT_EQ(xAiQueryTurn(nullptr), 0);

  /* xAiQueryCancel on NULL is a silent no-op. */
  xAiQueryCancel(nullptr);

  /* xAiQueryDestroy on NULL is a silent no-op. */
  xAiQueryDestroy(nullptr);

  /* xAiQueryUsage on NULL must populate the out struct with
   * all-(-1) sentinels so callers can rely on a single shape. */
  xAiUsage u{0, 0, 0};
  xAiQueryUsage(nullptr, &u);
  EXPECT_EQ(u.prompt_tokens, -1);
  EXPECT_EQ(u.completion_tokens, -1);
  EXPECT_EQ(u.total_tokens, -1);

  /* NULL out pointer must not crash and must not write anywhere. */
  xAiQueryUsage(nullptr, nullptr);
  SUCCEED();
}

TEST(QueryPublicApi, CreateRejectsNullArgs) {
  /* Both args required; Create returns NULL on any omission. No
   * Session is actually live here, but the early-return branches
   * must trigger before any Session member is dereferenced. */
  xAiQueryConf conf{};
  EXPECT_EQ(xAiQueryCreate(nullptr, &conf), nullptr);
  /* A non-NULL Session with a NULL conf is also rejected — we
   * don't have a real Session to hand in, but xAiSession is an
   * opaque handle so any non-NULL pointer exercises the NULL-conf
   * branch (the arg check runs before any dereference). */
  auto dummy_sess = reinterpret_cast<xAiSession>(0x1);
  EXPECT_EQ(xAiQueryCreate(dummy_sess, nullptr), nullptr);
}

TEST(QueryPublicApi, RunRejectsNullQuery) {
  EXPECT_EQ(xAiQueryRun(nullptr, xAiMessageFromText("hi")),
            xErrno_InvalidArg);
}
