/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent_test.cpp - Unit tests for xAiAgent bookkeeping.
 *
 * Agent is pure state: every field in xAiAgentConf is captured by
 * value, nothing is mutated after Create, and Destroy just frees
 * the struct. These tests cover:
 *
 *   - argument validation on Create (NULL conf / loop / provider /
 *     n_tools>0 with NULL tools),
 *   - field-by-field capture into struct xAiAgent_ (reached through
 *     agent_private.h, same pattern as provider_test.cpp),
 *   - Destroy tolerates NULL and does not touch the borrowed
 *     provider / tools.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xai/agent.h>
#include <xai/provider.h>
#include <xai/tool.h>
#include <xbase/event.h>
#include "agent_private.h"
#include "provider_private.h"
}

#include <cstdlib>

/* ── Minimal no-op provider (agent never actually calls it) ────────── */

static xErrno noop_submit(void *, const xAiProviderSubmitConf *,
                          const xAiProviderStreamCallbacks *, void *) {
  return xErrno_Ok;
}
static void noop_cancel(void *) {}
static void noop_destroy(void *) {}

static const xAiProviderVtable kNoopVtable = {
  noop_submit, noop_cancel, noop_destroy,
};

static xAiProvider make_noop_provider() {
  auto *base = static_cast<xAiProvider_ *>(calloc(1, sizeof(xAiProvider_)));
  base->vt   = &kNoopVtable;
  base->ctx  = nullptr;
  return reinterpret_cast<xAiProvider>(base);
}

/* Tool handler used below; xAiToolConf.handler has C linkage so we
 * cannot inline a lambda portably. */
static xErrno noop_tool_handler(const xAiContent *, xAiContent *, void *) {
  return xErrno_Ok;
}

/* ── Fixture ──────────────────────────────────────────────────────── */

class AgentTest : public ::testing::Test {
protected:
  xEventLoop  loop = nullptr;
  xAiProvider pvd  = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    pvd = make_noop_provider();
    ASSERT_NE(pvd, nullptr);
  }

  void TearDown() override {
    if (pvd)  xAiProviderDestroy(pvd);
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ── Argument validation ─────────────────────────────────────────── */

TEST_F(AgentTest, CreateRejectsNullConf) {
  EXPECT_EQ(xAiAgentCreate(nullptr), nullptr);
}

TEST_F(AgentTest, CreateRejectsMissingLoop) {
  xAiAgentConf conf = {};
  conf.provider = pvd;
  EXPECT_EQ(xAiAgentCreate(&conf), nullptr);
}

TEST_F(AgentTest, CreateRejectsMissingProvider) {
  xAiAgentConf conf = {};
  conf.loop = loop;
  EXPECT_EQ(xAiAgentCreate(&conf), nullptr);
}

TEST_F(AgentTest, CreateRejectsNonZeroToolsWithNullArray) {
  /* n_tools > 0 with tools == NULL is a caller bug; catch it at the
   * door so session.c never has to guard. */
  xAiAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  conf.tools    = nullptr;
  conf.n_tools  = 3;
  EXPECT_EQ(xAiAgentCreate(&conf), nullptr);
}

/* ── Happy path: minimal valid conf ──────────────────────────────── */

TEST_F(AgentTest, CreateWithMinimalConfSucceedsAndZerosOptionals) {
  xAiAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;

  xAiAgent ag = xAiAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  auto *a = reinterpret_cast<struct xAiAgent_ *>(ag);
  EXPECT_EQ(a->loop,           loop);
  EXPECT_EQ(a->provider,       pvd);
  EXPECT_EQ(a->model,          nullptr);
  EXPECT_EQ(a->system_prompt,  nullptr);
  EXPECT_EQ(a->tools,          nullptr);
  EXPECT_EQ(a->n_tools,        0u);
  EXPECT_EQ(a->task_group,     nullptr);
  EXPECT_EQ(a->max_turns,      0);
  EXPECT_EQ(a->max_tokens,     0);
  EXPECT_EQ(a->context_budget, 0u);

  xAiAgentDestroy(ag);
}

/* ── Full capture: every field round-trips ───────────────────────── */

TEST_F(AgentTest, CreateCapturesEveryField) {
  /* One real tool is enough to pin the "tools array is borrowed and
   * n_tools mirrors the count" contract. */
  xAiToolConf tconf = {};
  tconf.name        = "noop";
  tconf.description = "does nothing";
  tconf.handler     = noop_tool_handler;
  xAiTool t = xAiToolCreate(&tconf);
  ASSERT_NE(t, nullptr);

  const xAiTool *tools[] = {&t};

  xAiAgentConf conf = {};
  conf.loop           = loop;
  conf.provider       = pvd;
  conf.model          = "kimi-k2.6";
  conf.system_prompt  = "be concise";
  conf.tools          = tools;
  conf.n_tools        = 1;
  conf.task_group     = nullptr;   /* can't cheaply construct one here */
  conf.max_turns      = 7;
  conf.max_tokens     = 512;
  conf.context_budget = 32 * 1024;

  xAiAgent ag = xAiAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  auto *a = reinterpret_cast<struct xAiAgent_ *>(ag);

  /* Pointers are borrowed — check identity, not content. */
  EXPECT_EQ(a->loop,          loop);
  EXPECT_EQ(a->provider,      pvd);
  EXPECT_EQ(a->model,         conf.model);
  EXPECT_EQ(a->system_prompt, conf.system_prompt);
  EXPECT_EQ(a->tools,         tools);
  EXPECT_EQ(a->n_tools,       1u);

  /* Scalars round-trip. */
  EXPECT_EQ(a->max_turns,      7);
  EXPECT_EQ(a->max_tokens,     512);
  EXPECT_EQ(a->context_budget, static_cast<size_t>(32 * 1024));

  /* Destroying the agent must not touch borrowed dependencies —
   * the tool still works afterwards. */
  xAiAgentDestroy(ag);
  xAiToolDestroy(t);
}

/* ── Destroy ─────────────────────────────────────────────────────── */

TEST_F(AgentTest, DestroyNullIsNoop) {
  xAiAgentDestroy(nullptr); /* must not crash */
}
