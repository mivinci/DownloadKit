/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_test.cpp - Coverage for xAiSession (MVP text-only path)
 *
 * These tests drive xAiSession through a programmable fake provider
 * that records every submit() it receives and lets each test script
 * what the provider should stream back. That keeps the suite
 * hermetic (no HTTP, no sockets) and lets us observe the exact
 * messages[] view the session builds.
 */

extern "C" {
#include "agent_private.h"
#include "provider_private.h"
#include "session_private.h"

#include <xai/agent.h>
#include <xai/message.h>
#include <xai/provider.h>
#include <xai/session.h>
#include <xai/tool.h>
#include <xbase/error.h>
#include <xbase/event.h>
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

/* ── Fake provider ──────────────────────────────────────────────────── */

struct FakeScript {
  enum Kind { TEXT, TOOL_CALL, DONE };
  Kind                  kind;
  std::string           text;
  xAiProviderStopReason reason;
  xErrno                err;
};

struct FakeCapturedMsg {
  xAiRole     role;
  std::string text;
};

struct FakeImpl {
  /* Scripted behaviour (set by the test before calling Input). */
  std::vector<FakeScript> script;
  /* Captures of what submit() saw (for assertions). */
  std::vector<FakeCapturedMsg> captured_msgs;
  int                          submits   = 0;
  int                          cancels   = 0;
  int                          destroyed = 0;
  /* When set, submit() returns this code immediately (no callbacks). */
  xErrno submit_return = xErrno_Ok;

  /* Stashed streaming callbacks — lets a test deliver on_done after
   * submit() returns (simulating async providers). */
  xAiProviderStreamCallbacks cbs_last = {};
  void                      *cb_arg_last = nullptr;
  /* If non-zero, cancel() immediately delivers on_done(Cancelled). */
  int cancel_fires_done = 0;
};

static xErrno fake_submit(void *impl, const xAiProviderSubmitConf *conf,
                          const xAiProviderStreamCallbacks *cbs, void *cb_arg) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->submits++;
  f->cbs_last    = *cbs;
  f->cb_arg_last = cb_arg;
  f->captured_msgs.clear();
  for (size_t i = 0; i < conf->n_messages; i++) {
    FakeCapturedMsg m;
    m.role = conf->messages[i].role;
    for (size_t j = 0; j < conf->messages[i].n; j++) {
      const xAiContent &c = conf->messages[i].contents[j];
      if (c.type == xAiContentType_Text) {
        m.text.append(c.u.text.text, c.u.text.len);
      }
    }
    f->captured_msgs.push_back(std::move(m));
  }

  if (f->submit_return != xErrno_Ok) return f->submit_return;

  /* Replay scripted events synchronously — session.c has to cope. */
  for (const auto &ev : f->script) {
    switch (ev.kind) {
      case FakeScript::TEXT:
        if (cbs->on_text) cbs->on_text(ev.text.data(), ev.text.size(), cb_arg);
        break;
      case FakeScript::TOOL_CALL:
        if (cbs->on_tool_call) {
          xAiContent tc = {};
          tc.type                = xAiContentType_ToolUse;
          tc.u.tool_use.id       = "call_1";
          tc.u.tool_use.name     = ev.text.c_str();
          tc.u.tool_use.args_json = "{}";
          cbs->on_tool_call(&tc, cb_arg);
        }
        break;
      case FakeScript::DONE:
        if (cbs->on_done) cbs->on_done(ev.reason, ev.err, cb_arg);
        break;
    }
  }
  return xErrno_Ok;
}

static void fake_cancel(void *impl) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->cancels++;
  if (f->cancel_fires_done && f->cbs_last.on_done) {
    f->cbs_last.on_done(xAiProviderStop_Cancelled, xErrno_Ok, f->cb_arg_last);
  }
}

static void fake_destroy(void *impl) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->destroyed = 1;
  delete f;
}

static const xAiProviderVtable kFakeVtable = {fake_submit, fake_cancel,
                                              fake_destroy};

static xAiProvider make_fake_provider(FakeImpl **out) {
  auto *impl = new FakeImpl();
  auto *base = static_cast<xAiProvider_ *>(calloc(1, sizeof(xAiProvider_)));
  base->vt   = &kFakeVtable;
  base->ctx  = impl;
  *out       = impl;
  return reinterpret_cast<xAiProvider>(base);
}

/* ── Callback capture for the session side ─────────────────────────── */

struct Captured {
  std::string   texts;
  int           texts_fired = 0;
  int           done_fired  = 0;
  xAiDoneReason done_reason = xAiDoneReason_Completed;
  int           error_fired = 0;
  xErrno        error_code  = xErrno_Ok;
  std::string   error_msg;
};

static void cb_text(xAiSession, const char *c, size_t n, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->texts.append(c, n);
  cap->texts_fired++;
}
static void cb_done(xAiSession, xAiDoneReason r, void *ud) {
  auto *cap        = static_cast<Captured *>(ud);
  cap->done_fired++;
  cap->done_reason = r;
}
static void cb_err(xAiSession, xErrno e, const char *m, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->error_fired++;
  cap->error_code = e;
  if (m) cap->error_msg = m;
}

/* ── Fixture ────────────────────────────────────────────────────────── */

class SessionTest : public ::testing::Test {
 protected:
  xEventLoop  loop_     = nullptr;
  xAiProvider provider_ = nullptr;
  FakeImpl   *fake_     = nullptr;
  xAiAgent    agent_    = nullptr;

  void SetUp() override {
    loop_     = xEventLoopCreate();
    provider_ = make_fake_provider(&fake_);

    xAiAgentConf ac    = {};
    ac.loop            = loop_;
    ac.provider        = provider_;
    ac.model           = "fake-model";
    ac.system_prompt   = "you are a test";
    ac.max_turns       = 5;
    ac.max_tokens      = 1024;
    ac.context_budget  = 8192;
    agent_             = xAiAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }
  void TearDown() override {
    xAiAgentDestroy(agent_);
    xAiProviderDestroy(provider_);
    xEventLoopDestroy(loop_);
  }

  xAiSession make_session(const xAiSessionCallbacks &cbs) {
    xAiSessionConf sc = {};
    sc.cbs            = cbs;
    return xAiSessionCreate(agent_, &sc);
  }

  static xAiSessionCallbacks make_cbs(Captured *cap) {
    xAiSessionCallbacks c = {};
    c.on_text             = cb_text;
    c.on_done             = cb_done;
    c.on_error            = cb_err;
    c.user_data           = cap;
    return c;
  }
};

/* ── Lifecycle / arg validation ─────────────────────────────────────── */

TEST_F(SessionTest, CreateRejectsNullAgent) {
  xAiSessionConf sc = {};
  EXPECT_EQ(xAiSessionCreate(nullptr, &sc), nullptr);
}

TEST_F(SessionTest, CreateRejectsNullConf) {
  EXPECT_EQ(xAiSessionCreate(agent_, nullptr), nullptr);
}

TEST_F(SessionTest, DestroyAcceptsNull) {
  xAiSessionDestroy(nullptr); /* must not crash */
}

TEST_F(SessionTest, CreateInheritsSystemPromptAndModel) {
  xAiSessionConf sc = {};
  xAiSession sess   = xAiSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  EXPECT_STREQ(s->system_prompt, "you are a test");
  EXPECT_STREQ(s->model, "fake-model");
  EXPECT_EQ(s->max_turns, 5);
  EXPECT_EQ(s->max_tokens, 1024);
  EXPECT_EQ(s->context_budget, 8192u);
  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, CreateOverridesWinOverAgent) {
  xAiSessionConf sc  = {};
  sc.system_prompt   = "override-sp";
  sc.model           = "override-model";
  sc.max_turns       = 99;
  xAiSession sess    = xAiSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  EXPECT_STREQ(s->system_prompt, "override-sp");
  EXPECT_STREQ(s->model, "override-model");
  EXPECT_EQ(s->max_turns, 99);
  xAiSessionDestroy(sess);
}

/* ── Text-only happy path ───────────────────────────────────────────── */

TEST_F(SessionTest, TextOnlyRoundDeliversTextAndDone) {
  Captured cap;
  auto cbs  = make_cbs(&cap);
  xAiSession sess = make_session(cbs);
  ASSERT_NE(sess, nullptr);

  fake_->script = {
      {FakeScript::TEXT, "hello ", {}, {}},
      {FakeScript::TEXT, "world", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok},
  };

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.texts_fired, 2);
  EXPECT_EQ(cap.texts, "hello world");
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);
  EXPECT_EQ(cap.error_fired, 0);

  /* History: system is not stored, so we expect user + assistant. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  ASSERT_EQ(s->n_history, 2u);
  EXPECT_EQ(s->history[0].role, xAiRole_User);
  EXPECT_STREQ(s->history[0].text, "hi");
  EXPECT_EQ(s->history[1].role, xAiRole_Assistant);
  EXPECT_STREQ(s->history[1].text, "hello world");

  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, SubmitViewPrependsSystemPrompt) {
  Captured cap;
  auto cbs  = make_cbs(&cap);
  xAiSession sess = make_session(cbs);
  fake_->script = {{FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok}};

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  ASSERT_EQ(fake_->submits, 1);
  ASSERT_GE(fake_->captured_msgs.size(), 2u);
  EXPECT_EQ(fake_->captured_msgs[0].role, xAiRole_System);
  EXPECT_EQ(fake_->captured_msgs[0].text, "you are a test");
  EXPECT_EQ(fake_->captured_msgs[1].role, xAiRole_User);
  EXPECT_EQ(fake_->captured_msgs[1].text, "hi");

  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, MultiTurnAccumulatesHistory) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script = {
      {FakeScript::TEXT, "round1", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("q1")), xErrno_Ok);

  fake_->script = {
      {FakeScript::TEXT, "round2", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("q2")), xErrno_Ok);

  /* 2 users + 2 assistants. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  ASSERT_EQ(s->n_history, 4u);
  EXPECT_STREQ(s->history[0].text, "q1");
  EXPECT_STREQ(s->history[1].text, "round1");
  EXPECT_STREQ(s->history[2].text, "q2");
  EXPECT_STREQ(s->history[3].text, "round2");

  /* Second submit sees system + [q1, round1, q2] = 4 messages.
   * The second assistant reply (round2) is only committed to history
   * after submit returns, so it can't be in the wire view. */
  EXPECT_EQ(fake_->captured_msgs.size(), 4u);
  EXPECT_EQ(fake_->captured_msgs[0].role, xAiRole_System);
  EXPECT_EQ(fake_->captured_msgs[1].text, "q1");
  EXPECT_EQ(fake_->captured_msgs[2].text, "round1");
  EXPECT_EQ(fake_->captured_msgs[3].text, "q2");
  xAiSessionDestroy(sess);
}

/* ── Stop reason translation ────────────────────────────────────────── */

TEST_F(SessionTest, ProviderErrorMapsToModelErrorAndFiresOnError) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script = {
      {FakeScript::TEXT, "partial", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_Error, xErrno_Again},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_ModelError);
  EXPECT_EQ(cap.error_fired, 1);
  EXPECT_EQ(cap.error_code, xErrno_Again);

  /* Partial assistant text still makes it into history. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  ASSERT_EQ(s->n_history, 2u);
  EXPECT_STREQ(s->history[1].text, "partial");

  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, ProviderStopSeqMapsToStopped) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));
  fake_->script   = {
      {FakeScript::DONE, "", xAiProviderStop_StopSeq, xErrno_Ok}};
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Stopped);
  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, ProviderPromptLongMapsToPromptTooLong) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));
  fake_->script   = {
      {FakeScript::DONE, "", xAiProviderStop_PromptLong, xErrno_Ok}};
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_PromptTooLong);
  xAiSessionDestroy(sess);
}

/* ── tool_use path: MVP rejects with ToolError ──────────────────────── */

TEST_F(SessionTest, ToolCallUnsupportedInMvpReportsToolError) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script = {
      {FakeScript::TEXT, "hmm ", {}, {}},
      {FakeScript::TOOL_CALL, "get_time", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_ToolUse, xErrno_Ok},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("what time is it?")),
            xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_ToolError);
  EXPECT_EQ(cap.error_fired, 1);
  EXPECT_EQ(cap.error_code, xErrno_NotSupported);
  EXPECT_NE(cap.error_msg.find("tool_use"), std::string::npos);

  xAiSessionDestroy(sess);
}

/* ── Busy: re-entrant Input during a run is rejected ────────────────── */

struct ReInputCap {
  xAiSession sess  = nullptr;
  xErrno     rc    = xErrno_Ok;
  int        fired = 0;
};

static void cb_reenter_text(xAiSession sess, const char *, size_t, void *ud) {
  auto *r = static_cast<ReInputCap *>(ud);
  if (r->fired++) return; /* only try once */
  /* Try to recursively call Input while we are mid-stream. */
  r->rc = xAiSessionInput(sess, xAiMessageFromText("nested"));
}
static void cb_reenter_done(xAiSession, xAiDoneReason, void *) {}

TEST_F(SessionTest, ReentrantInputReturnsBusy) {
  ReInputCap              r;
  xAiSessionCallbacks cbs = {};
  cbs.on_text             = cb_reenter_text;
  cbs.on_done             = cb_reenter_done;
  cbs.user_data           = &r;

  xAiSessionConf sc = {};
  sc.cbs            = cbs;
  xAiSession sess   = xAiSessionCreate(agent_, &sc);
  r.sess            = sess;

  fake_->script = {
      {FakeScript::TEXT, "hello", {}, {}},
      {FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(r.rc, xErrno_Busy);
  xAiSessionDestroy(sess);
}

/* ── Cancel: provider_cancel fires; on_done reports Aborted ─────────── */

TEST_F(SessionTest, CancelBeforeDoneMapsToAborted) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  /* Script only delivers text — fake's on_done arrives via cancel. */
  fake_->cancel_fires_done = 1;
  fake_->script            = {
      {FakeScript::TEXT, "partial", {}, {}},
  };
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 0); /* no DONE scripted yet */

  xAiSessionCancel(sess);
  EXPECT_EQ(fake_->cancels, 1);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Aborted);

  /* Partial text preserved in history. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  ASSERT_EQ(s->n_history, 2u);
  EXPECT_STREQ(s->history[1].text, "partial");

  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, CancelOnIdleSessionIsNoOp) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));
  xAiSessionCancel(sess); /* running == 0, must be a silent no-op */
  EXPECT_EQ(fake_->cancels, 0);
  EXPECT_EQ(cap.done_fired, 0);
  xAiSessionDestroy(sess);
}

/* ── Submit failure is propagated and does not leave running set ────── */

TEST_F(SessionTest, SubmitFailureRollsBackAndReturnsError) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->submit_return = xErrno_Again;

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Again);

  /* on_done should NOT have fired — we never started. */
  EXPECT_EQ(cap.done_fired, 0);

  /* History rolled back. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  EXPECT_EQ(s->n_history, 0u);
  EXPECT_EQ(s->running, 0);

  /* And a fresh attempt is allowed. */
  fake_->submit_return = xErrno_Ok;
  fake_->script        = {
      {FakeScript::DONE, "", xAiProviderStop_EndTurn, xErrno_Ok}};
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("retry")), xErrno_Ok);

  xAiSessionDestroy(sess);
}
