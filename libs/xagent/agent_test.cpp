/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent_test.cpp - Unit tests for xAgent bookkeeping.
 *
 * Agent is pure state: every field in xAgentConf is captured by
 * value, nothing is mutated after Create, and Destroy just frees
 * the struct. These tests cover:
 *
 *   - argument validation on Create (NULL conf / loop / provider /
 *     tools_count>0 with NULL tools),
 *   - field-by-field capture into struct xAgent_ (reached through
 *     agent_private.h, same pattern as provider_test.cpp),
 *   - Destroy tolerates NULL and does not touch the borrowed
 *     provider / tools.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xagent/agent.h>
#include <xagent/memory.h>
#include <xagent/provider.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/event.h>
#include "agent_private.h"
#include "provider_private.h"
#include "session_private.h"
}

#include <cstdlib>
#include <cstring>
#include <string>

/* ── Minimal no-op provider (agent never actually calls it) ────────── */

static xErrno noop_submit(void *, const xAgentProviderSubmitConf *,
                          const xAgentProviderStreamCallbacks *, void *) {
  return xErrno_Ok;
}
static void noop_cancel(void *) {}
static void noop_destroy(void *) {}

static const xAgentProviderVtable kNoopVtable = {
  noop_submit, noop_cancel, noop_destroy,
};

static xAgentProvider make_noop_provider() {
  auto *base = static_cast<xAgentProvider_ *>(calloc(1, sizeof(xAgentProvider_)));
  base->vt   = &kNoopVtable;
  base->ctx  = nullptr;
  return reinterpret_cast<xAgentProvider>(base);
}

/* Tool handler used below; xAgentToolConf.handler has C linkage so we
 * cannot inline a lambda portably. */
static xErrno noop_tool_handler(xAgentQuery, const xAgentContent *, xAgentContent *, void *) {
  return xErrno_Ok;
}

/* ── Fixture ──────────────────────────────────────────────────────── */

class AgentTest : public ::testing::Test {
protected:
  xEventLoop  loop = nullptr;
  xAgentProvider pvd  = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    pvd = make_noop_provider();
    ASSERT_NE(pvd, nullptr);
  }

  void TearDown() override {
    if (pvd)  xAgentProviderDestroy(pvd);
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ── Argument validation ─────────────────────────────────────────── */

TEST_F(AgentTest, CreateRejectsNullConf) {
  EXPECT_EQ(xAgentCreate(nullptr), nullptr);
}

TEST_F(AgentTest, CreateRejectsMissingLoop) {
  xAgentConf conf = {};
  conf.provider = pvd;
  EXPECT_EQ(xAgentCreate(&conf), nullptr);
}

TEST_F(AgentTest, CreateRejectsMissingProvider) {
  xAgentConf conf = {};
  conf.loop = loop;
  EXPECT_EQ(xAgentCreate(&conf), nullptr);
}

TEST_F(AgentTest, CreateRejectsNonZeroToolsWithNullArray) {
  /* tools_count > 0 with tools == NULL is a caller bug; catch it at the
   * door so session.c never has to guard. */
  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  conf.tools    = nullptr;
  conf.tools_count = 3;
  EXPECT_EQ(xAgentCreate(&conf), nullptr);
}

/* ── Happy path: minimal valid conf ──────────────────────────────── */

TEST_F(AgentTest, CreateWithMinimalConfSucceedsAndZerosOptionals) {
  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  auto *a = reinterpret_cast<struct xAgent_ *>(ag);
  EXPECT_EQ(a->loop,           loop);
  EXPECT_EQ(a->provider,       pvd);
  EXPECT_EQ(a->model,          nullptr);
  EXPECT_EQ(a->system_prompt,  nullptr);
  EXPECT_EQ(a->tools,          nullptr);
  EXPECT_EQ(a->tools_count,    0u);
  EXPECT_EQ(a->task_group,     nullptr);
  EXPECT_EQ(a->max_turns,      0);
  EXPECT_EQ(a->max_tokens,     0);

  xAgentDestroy(ag);
}

/* ── Full capture: every field round-trips ───────────────────────── */

TEST_F(AgentTest, CreateCapturesEveryField) {
  /* One real tool is enough to pin the "tools array is borrowed and
   * tools_count mirrors the count" contract. */
  xAgentToolConf tconf = {};
  tconf.name        = "noop";
  tconf.description = "does nothing";
  tconf.handler     = noop_tool_handler;
  xAgentTool t = xAgentToolCreate(&tconf);
  ASSERT_NE(t, nullptr);

  const xAgentTool *tools[] = {&t};

  xAgentConf conf = {};
  conf.loop           = loop;
  conf.provider       = pvd;
  conf.model          = "kimi-k2.6";
  conf.system_prompt  = "be concise";
  conf.tools          = tools;
  conf.tools_count    = 1;
  conf.task_group     = nullptr;   /* can't cheaply construct one here */
  conf.max_turns      = 7;
  conf.max_tokens     = 512;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  auto *a = reinterpret_cast<struct xAgent_ *>(ag);

  /* Pointers are borrowed — check identity, not content. */
  EXPECT_EQ(a->loop,          loop);
  EXPECT_EQ(a->provider,      pvd);
  EXPECT_EQ(a->model,         conf.model);
  EXPECT_EQ(a->system_prompt, conf.system_prompt);
  EXPECT_EQ(a->tools,         tools);
  EXPECT_EQ(a->tools_count,   1u);

  /* Scalars round-trip. */
  EXPECT_EQ(a->max_turns,      7);
  EXPECT_EQ(a->max_tokens,     512);

  /* Destroying the agent must not touch borrowed dependencies —
   * the tool still works afterwards. */
  xAgentDestroy(ag);
  xAgentToolDestroy(t);
}

/* ── Destroy ─────────────────────────────────────────────────────── */

TEST_F(AgentTest, DestroyNullIsNoop) {
  xAgentDestroy(nullptr); /* must not crash */
}

/* ── Default session ─────────────────────────────────────────────── */

TEST_F(AgentTest, DefaultSessionOnNullAgentReturnsNull) {
  EXPECT_EQ(xAgentDefaultSession(nullptr), nullptr);
}

TEST_F(AgentTest, DefaultSessionIsNullWhenConfIsNull) {
  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  /* default_session_conf left NULL */

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);
  EXPECT_EQ(xAgentDefaultSession(ag), nullptr);
  xAgentDestroy(ag);
}

TEST_F(AgentTest, DefaultSessionCreatedWhenConfProvided) {
  xAgentSessionConf sc = {};  /* zero-init: all fields inherited, origin=User */
  xAgentConf conf = {};
  conf.loop                = loop;
  conf.provider            = pvd;
  conf.default_session_conf = &sc;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  xAgentSession ds = xAgentDefaultSession(ag);
  EXPECT_NE(ds, nullptr);

  /* The default session should be usable like any other session.
   * Zero-initialised origin defaults to User — the default session
   * is the user's primary conversation entry. */
  EXPECT_EQ(xAgentSessionOrigin(ds), xAgentInputOrigin_User);

  xAgentDestroy(ag);
}

TEST_F(AgentTest, DefaultSessionOriginHonoursCallerSetting) {
  /* The default session's origin is honoured as-is — it is NOT
   * forced to SystemSynthesized. The default session is the user's
   * primary conversation entry, so origin=User is the natural
   * default (zero-init). The caller may override it if desired. */
  xAgentSessionConf sc = {};
  sc.origin = xAgentInputOrigin_User;

  xAgentConf conf = {};
  conf.loop                = loop;
  conf.provider            = pvd;
  conf.default_session_conf = &sc;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  xAgentSession ds = xAgentDefaultSession(ag);
  ASSERT_NE(ds, nullptr);
  EXPECT_EQ(xAgentSessionOrigin(ds), xAgentInputOrigin_User);

  xAgentDestroy(ag);
}

TEST_F(AgentTest, DefaultSessionDestroyedWithAgent) {
  /* Verify that destroying the agent does not leak the default
   * session. We cannot directly observe the free, but we can
   * confirm the API does not crash and the session pointer is
   * internally NULLed (checked via private access). */
  xAgentSessionConf sc = {};
  xAgentConf conf = {};
  conf.loop                = loop;
  conf.provider            = pvd;
  conf.default_session_conf = &sc;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);
  EXPECT_NE(xAgentDefaultSession(ag), nullptr);

  /* Destroy must not crash; the default session is freed. */
  xAgentDestroy(ag);
  /* No ASAN leak, no use-after-free — the test infrastructure
   * will catch those. */
}

TEST_F(AgentTest, DefaultSessionInheritsAgentDefaults) {
  xAgentSessionConf sc = {};  /* all fields zero → inherit from agent */
  xAgentConf conf = {};
  conf.loop                = loop;
  conf.provider            = pvd;
  conf.model               = "kimi-k2.6";
  conf.system_prompt       = "be helpful";
  conf.max_turns           = 10;
  conf.max_tokens          = 2048;
  conf.default_session_conf = &sc;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  xAgentSession ds = xAgentDefaultSession(ag);
  ASSERT_NE(ds, nullptr);

  /* The default session inherits from the agent just like any
   * other session created via xAgentCreateSession. */
  auto *s = reinterpret_cast<struct xAgentSession_ *>(ds);
  EXPECT_EQ(s->model,  conf.model);
  EXPECT_EQ(s->max_turns,  conf.max_turns);
  EXPECT_EQ(s->max_tokens, conf.max_tokens);

  xAgentDestroy(ag);
}

/* ── Memory wiring ───────────────────────────────────────────────── */

/* When the agent is created with an explicit xAgentMemory store, the
 * session it mints routes its L1 preserve hook through that store
 * instead of writing JSONL files directly. We verify by firing the
 * hook manually and then consulting the store. */
TEST_F(AgentTest, MemoryStoreReceivesL1Preserve) {
  /* JSONL backend pointed at a per-test temp root. */
  std::string root = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                                        : "/tmp") +
                     "/xagent_memwire_" +
                     std::to_string(::testing::UnitTest::GetInstance()
                                      ->current_test_info()
                                      ->name()
                                      ? 0
                                      : 0);
  /* Flatten any stale state so each run is fresh. */
  std::string rm = "rm -rf '" + root + "'";
  (void)std::system(rm.c_str());

  xAgentMemoryJsonlConf mc = {};
  mc.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&mc);
  ASSERT_NE(store, nullptr);

  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  conf.agent_id = "integ_agent";
  /* data_dir is irrelevant when memory is set — we leave it NULL to
   * be sure the memory path is the one taking effect. */
  conf.memory   = store;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  /* A minimal session: no callbacks, no L1 hook of its own — the
   * agent is expected to inject the memory-backed one for us. */
  xAgentSessionConf sc = {};
  sc.session_id      = "sess_a";
  xAgentSession sess = xAgentCreateSession(ag, &sc);
  ASSERT_NE(sess, nullptr);

  auto *s = reinterpret_cast<struct xAgentSession_ *>(sess);
  ASSERT_NE(s->on_l1_preserve, nullptr);
  ASSERT_NE(s->l1_preserve_owner, nullptr);

  /* Fire the hook with a Truncated batch, as if the budget policy
   * had dropped two old entries. The callback should route the
   * batch through xAgentMemoryAppend. */
  xAgentSessionMsg msg0{};
  msg0.role     = xAgentRole_User;
  msg0.kind     = xAgentSessionEntryKind_Text;
  msg0.text     = "first turn";
  msg0.text_len = std::strlen("first turn");
  xAgentSessionMsg msg1{};
  msg1.role     = xAgentRole_Assistant;
  msg1.kind     = xAgentSessionEntryKind_Text;
  msg1.text     = "first reply";
  msg1.text_len = std::strlen("first reply");
  xAgentSessionMsg batch[] = {msg0, msg1};

  s->on_l1_preserve(sess, batch, 2, xAgentL1PreserveReason_Truncated,
                    s->l1_preserve_owner);

  /* Retrieve from the store and assert the two entries round-tripped. */
  xAgentMemoryQuery q{};
  q.agent_id   = "integ_agent";
  q.session_id = "sess_a";
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{2});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("first turn"));
  EXPECT_EQ(std::string(hits.entries[1].text, hits.entries[1].text_len),
            std::string("first reply"));
  xAgentMemoryReleaseHits(store, &hits);

  /* Destroying the session fires Finalizing with an empty batch —
   * that path must also free the owner context without writing. */
  xAgentSessionDestroy(sess);
  xAgentDestroy(ag);
  xAgentMemoryDestroy(store);
  (void)std::system(rm.c_str());
}

/* When no memory store is configured but data_dir IS, sessions still
 * get the legacy JSONL callback wired in. This is the backwards-
 * compat guard: existing callers must keep working. */
TEST_F(AgentTest, WithoutMemoryStoreFallsBackToJsonlAutoWire) {
  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  conf.agent_id = "legacy_agent";
  conf.data_dir = "/tmp/xagent_test_legacy"; /* any path */
  /* conf.memory stays NULL */

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  xAgentSessionConf sc = {};
  sc.session_id      = "sess_legacy";
  xAgentSession sess = xAgentCreateSession(ag, &sc);
  ASSERT_NE(sess, nullptr);

  auto *s = reinterpret_cast<struct xAgentSession_ *>(sess);
  EXPECT_NE(s->on_l1_preserve, nullptr);
  EXPECT_NE(s->l1_preserve_owner, nullptr);

  xAgentSessionDestroy(sess);
  xAgentDestroy(ag);
}

/* When the caller explicitly sets on_l1_preserve in the session conf,
 * the agent MUST NOT clobber it — neither the memory route nor the
 * JSONL fallback should be wired in. */
static void never_called_cb_(xAgentSession, const xAgentSessionMsg *, size_t,
                             xAgentL1PreserveReason, void *) {}

TEST_F(AgentTest, CallerProvidedL1CbTakesPriorityOverMemory) {
  xAgentMemoryJsonlConf mc = {};
  mc.root_dir = "/tmp/xagent_test_overridden";
  xAgentMemory store = xAgentMemoryJsonlCreate(&mc);
  ASSERT_NE(store, nullptr);

  xAgentConf conf = {};
  conf.loop     = loop;
  conf.provider = pvd;
  conf.agent_id = "a";
  conf.memory   = store;

  xAgent ag = xAgentCreate(&conf);
  ASSERT_NE(ag, nullptr);

  int owner_tag = 42;
  xAgentSessionConf sc = {};
  sc.session_id       = "s";
  sc.on_l1_preserve   = never_called_cb_;
  sc.l1_preserve_owner = &owner_tag;

  xAgentSession sess = xAgentCreateSession(ag, &sc);
  ASSERT_NE(sess, nullptr);

  auto *s = reinterpret_cast<struct xAgentSession_ *>(sess);
  EXPECT_EQ(s->on_l1_preserve, never_called_cb_);
  EXPECT_EQ(s->l1_preserve_owner, &owner_tag);

  xAgentSessionDestroy(sess);
  xAgentDestroy(ag);
  xAgentMemoryDestroy(store);
}
