/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_test.cpp - Whitebox tests for the xAgentQuery public API.
 *
 * Rationale:
 *   End-to-end Query behaviour (provider round, tool loop, usage
 *   accumulation, cancellation, Create/Run/Destroy lifecycle) is
 *   exercised through the public xAgentSession surface in
 *   session_test.cpp. These tests only cover what that suite can't
 *   reach trivially: the NULL-safety and out-parameter contracts
 *   of the standalone xAgentQuery* accessors.
 *
 *   Anything that needs a real Agent + provider stack is tested
 *   via xAgentSessionInput in session_test.cpp; duplicating that
 *   setup here would test the scaffolding, not the Query.
 */

extern "C" {
#include <x/agent/query.h>
#include <x/agent/session.h>
}

#include <gtest/gtest.h>

/* ── Public API NULL / out-param contracts ─────────────────────── */

TEST(QueryPublicApi, NullHandleIsSafe) {
  EXPECT_EQ(xAgentSessionQuery(nullptr), nullptr);
  EXPECT_EQ(xAgentQuerySession(nullptr), nullptr);
  EXPECT_EQ(xAgentQueryIsRunning(nullptr), 0);
  EXPECT_EQ(xAgentQueryTurn(nullptr), 0);

  /* xAgentQueryCancel on NULL is a silent no-op. */
  xAgentQueryCancel(nullptr);

  /* xAgentQueryDestroy on NULL is a silent no-op. */
  xAgentQueryDestroy(nullptr);

  /* xAgentQueryUsage on NULL must populate the out struct with
   * all-(-1) sentinels so callers can rely on a single shape. */
  xAgentUsage u{0, 0, 0};
  xAgentQueryUsage(nullptr, &u);
  EXPECT_EQ(u.prompt_tokens, -1);
  EXPECT_EQ(u.completion_tokens, -1);
  EXPECT_EQ(u.total_tokens, -1);

  /* NULL out pointer must not crash and must not write anywhere. */
  xAgentQueryUsage(nullptr, nullptr);
  SUCCEED();
}

TEST(QueryPublicApi, CreateRejectsNullConf) {
  /* NULL conf is rejected. */
  EXPECT_EQ(xAgentQueryCreate(nullptr), nullptr);

  /* Non-NULL conf with NULL provider is also rejected — a Query
   * cannot function without a provider. */
  xAgentQueryConf conf{};
  EXPECT_EQ(xAgentQueryCreate(&conf), nullptr);
}

TEST(QueryPublicApi, RunRejectsNullQuery) {
  xAgentMessage msg = xAgentMessageFromText("hi");
  EXPECT_EQ(xAgentQueryRun(nullptr, &msg, 1), xErrno_InvalidArg);
}

TEST(QueryPublicApi, RunRejectsEmptyInput) {
  /* A non-NULL Query + zero messages is rejected before the Query
   * is touched (the early-return branch runs on NULL msgs or n == 0). */
  auto dummy_q = reinterpret_cast<xAgentQuery>(0x1);
  EXPECT_EQ(xAgentQueryRun(dummy_q, nullptr, 0), xErrno_InvalidArg);
}
