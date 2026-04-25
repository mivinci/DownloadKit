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
  enum Kind { TEXT, TOOL_CALL, THINKING, DONE };
  Kind                  kind;
  std::string           text;      /* TEXT: payload; TOOL_CALL: name;
                                      THINKING: payload              */
  std::string           tool_id;   /* TOOL_CALL: id (default "call_1") */
  std::string           tool_args; /* TOOL_CALL: args_json (default "{}") */
  xAiProviderStopReason reason;
  xErrno                err;
  /* Optional per-round usage to hand the session along with DONE.
   * has_usage == false → provider reports NULL (server silent).
   * has_usage == true  → the three numbers below get passed; -1
   * still means "this field unknown" (the session accumulator is
   * expected to honour the sentinel). */
  bool                  has_usage         = false;
  int                   prompt_tokens     = -1;
  int                   completion_tokens = -1;
  int                   total_tokens      = -1;
};

/* Convenience constructors for scripted events. Keep the call sites
 * readable when a test only cares about two or three fields. */
static inline FakeScript SText(const char *payload) {
  FakeScript s{};
  s.kind = FakeScript::TEXT;
  s.text = payload;
  return s;
}
static inline FakeScript SDone(xAiProviderStopReason reason,
                               xErrno err = xErrno_Ok) {
  FakeScript s{};
  s.kind   = FakeScript::DONE;
  s.reason = reason;
  s.err    = err;
  return s;
}
static inline FakeScript SDoneWithUsage(xAiProviderStopReason reason,
                                        int prompt, int completion,
                                        int total = -1,
                                        xErrno err = xErrno_Ok) {
  FakeScript s{};
  s.kind              = FakeScript::DONE;
  s.reason            = reason;
  s.err               = err;
  s.has_usage         = true;
  s.prompt_tokens     = prompt;
  s.completion_tokens = completion;
  s.total_tokens      = total;
  return s;
}
static inline FakeScript SToolCall(const char *name,
                                   const char *id   = "call_1",
                                   const char *args = "{}") {
  FakeScript s{};
  s.kind      = FakeScript::TOOL_CALL;
  s.text      = name;
  s.tool_id   = id;
  s.tool_args = args;
  return s;
}
static inline FakeScript SThinking(const char *payload) {
  FakeScript s{};
  s.kind = FakeScript::THINKING;
  s.text = payload;
  return s;
}

struct FakeCapturedBlock {
  xAiContentType type;
  std::string    text;          /* Text / Thinking payload */
  std::string    tool_use_id;   /* ToolUse / ToolResult */
  std::string    tool_use_name;
  std::string    tool_use_args;
  std::string    tool_result_output;
  int            tool_result_is_error = 0;
};

struct FakeCapturedMsg {
  xAiRole                        role;
  std::string                    text; /* concatenated Text blocks */
  std::vector<FakeCapturedBlock> blocks;
};

struct FakeImpl {
  /* Scripted behaviour: one vector per expected submit(). If the
   * session submits more times than the queue has entries, the
   * extra submits replay the last queued script. The common single-
   * round case just pushes one entry. */
  std::vector<std::vector<FakeScript>> script_queue;
  /* Captures of what submit() saw (for assertions). One entry per
   * submit() call. captured_msgs always reflects the MOST RECENT
   * submit for backwards compatibility with existing tests. */
  std::vector<FakeCapturedMsg>              captured_msgs;
  std::vector<std::vector<FakeCapturedMsg>> captured_msgs_per_submit;
  int                                       submits   = 0;
  int                                       cancels   = 0;
  int                                       destroyed = 0;
  /* When set, submit() returns this code immediately (no callbacks). */
  xErrno submit_return = xErrno_Ok;

  /* Stashed streaming callbacks — lets a test deliver on_done after
   * submit() returns (simulating async providers). */
  xAiProviderStreamCallbacks cbs_last    = {};
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
      FakeCapturedBlock b;
      b.type = c.type;
      if (c.type == xAiContentType_Text) {
        b.text.assign(c.u.text.text, c.u.text.len);
        m.text.append(c.u.text.text, c.u.text.len);
      } else if (c.type == xAiContentType_Thinking) {
        b.text.assign(c.u.thinking.text, c.u.thinking.len);
      } else if (c.type == xAiContentType_ToolUse) {
        b.tool_use_id   = c.u.tool_use.id   ? c.u.tool_use.id   : "";
        b.tool_use_name = c.u.tool_use.name ? c.u.tool_use.name : "";
        b.tool_use_args =
            c.u.tool_use.args_json ? c.u.tool_use.args_json : "";
      } else if (c.type == xAiContentType_ToolResult) {
        b.tool_use_id = c.u.tool_result.id ? c.u.tool_result.id : "";
        if (c.u.tool_result.output) {
          b.tool_result_output.assign(c.u.tool_result.output,
                                      c.u.tool_result.output_len);
        }
        b.tool_result_is_error = c.u.tool_result.is_error;
      }
      m.blocks.push_back(std::move(b));
    }
    f->captured_msgs.push_back(std::move(m));
  }
  f->captured_msgs_per_submit.push_back(f->captured_msgs);

  if (f->submit_return != xErrno_Ok) return f->submit_return;

  /* Pick the script for this submit: consume the first queued list,
   * or re-use the most recent one if the queue is empty. */
  std::vector<FakeScript> script;
  if (!f->script_queue.empty()) {
    script = f->script_queue.front();
    f->script_queue.erase(f->script_queue.begin());
  }

  /* Replay scripted events synchronously — session.c has to cope. */
  for (const auto &ev : script) {
    switch (ev.kind) {
      case FakeScript::TEXT:
        if (cbs->on_text) cbs->on_text(ev.text.data(), ev.text.size(), cb_arg);
        break;
      case FakeScript::THINKING:
        if (cbs->on_thinking) {
          cbs->on_thinking(ev.text.data(), ev.text.size(), cb_arg);
        }
        break;
      case FakeScript::TOOL_CALL:
        if (cbs->on_tool_call) {
          xAiContent tc = {};
          tc.type                 = xAiContentType_ToolUse;
          tc.u.tool_use.id        = ev.tool_id.empty() ? "call_1"
                                                       : ev.tool_id.c_str();
          tc.u.tool_use.name      = ev.text.c_str();
          tc.u.tool_use.args_json = ev.tool_args.empty() ? "{}"
                                                         : ev.tool_args.c_str();
          cbs->on_tool_call(&tc, cb_arg);
        }
        break;
      case FakeScript::DONE:
        if (cbs->on_done) {
          xAiUsage u{ev.prompt_tokens, ev.completion_tokens, ev.total_tokens};
          cbs->on_done(ev.reason, ev.err, ev.has_usage ? &u : nullptr,
                       cb_arg);
        }
        break;
    }
  }
  return xErrno_Ok;
}

static void fake_cancel(void *impl) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->cancels++;
  if (f->cancel_fires_done && f->cbs_last.on_done) {
    f->cbs_last.on_done(xAiProviderStop_Cancelled, xErrno_Ok, nullptr,
                        f->cb_arg_last);
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
  std::string   thinking;       /* accumulated on_thinking deltas */
  int           thinking_fired = 0;
  int           done_fired  = 0;
  xAiDoneReason done_reason = xAiDoneReason_Completed;
  int           error_fired = 0;
  xErrno        error_code  = xErrno_Ok;
  std::string   error_msg;
  /* Usage snapshot from on_done. has_usage false when session hands
   * NULL (no round ever reported); otherwise holds the cumulative
   * totals across the whole run, with -1 for fields still unknown. */
  bool          has_usage   = false;
  xAiUsage      usage{-1, -1, -1};
};

static void cb_text(xAiSession, const char *c, size_t n, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->texts.append(c, n);
  cap->texts_fired++;
}
static void cb_thinking(xAiSession, const char *c, size_t n, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->thinking.append(c, n);
  cap->thinking_fired++;
}
static void cb_done(xAiSession, xAiDoneReason r, const xAiUsage *u, void *ud) {
  auto *cap        = static_cast<Captured *>(ud);
  cap->done_fired++;
  cap->done_reason = r;
  if (u) {
    cap->has_usage = true;
    cap->usage     = *u;
  }
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
    c.on_thinking         = cb_thinking;
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

/* ── Session-lifetime properties: origin + on_finalizing ─────────── */

TEST_F(SessionTest, OriginDefaultsToUserWhenUnset) {
  xAiSessionConf sc = {};
  xAiSession sess   = xAiSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  EXPECT_EQ(xAiSessionOrigin(sess), xAiInputOrigin_User);
  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, OriginEchoesConfValue) {
  xAiSessionConf sc = {};
  sc.origin         = xAiInputOrigin_SystemSynthesized;
  xAiSession sess   = xAiSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  EXPECT_EQ(xAiSessionOrigin(sess), xAiInputOrigin_SystemSynthesized);
  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, OriginOnNullSessionReturnsUser) {
  EXPECT_EQ(xAiSessionOrigin(nullptr), xAiInputOrigin_User);
}

/* on_finalizing must fire exactly once, with the configured owner,
 * while the session handle is still live (history still reachable). */
namespace {
struct FinalizingCap {
  int         calls       = 0;
  xAiSession  seen_sess   = nullptr;
  void       *seen_owner  = nullptr;
  size_t      history_len = SIZE_MAX;
};

void cb_finalizing(xAiSession sess, void *owner) {
  auto *cap = static_cast<FinalizingCap *>(owner);
  cap->calls++;
  cap->seen_sess   = sess;
  cap->seen_owner  = owner;
  /* Peek through the private layout to prove history isn't freed yet. */
  cap->history_len = reinterpret_cast<xAiSession_ *>(sess)->n_history;
}
}  // namespace

TEST_F(SessionTest, FinalizingHookFiresOnDestroyBeforeTeardown) {
  FinalizingCap cap;
  xAiSessionConf sc   = {};
  sc.on_finalizing    = cb_finalizing;
  sc.finalizing_owner = &cap;

  xAiSession sess = xAiSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  /* Append one user turn so history has something to observe. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAiProviderStop_EndTurn),
  });
  Captured dummy;
  auto cbs_noop = xAiSessionCallbacks{};
  cbs_noop.user_data = &dummy;
  reinterpret_cast<xAiSession_ *>(sess)->cbs = cbs_noop;
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.calls, 0);
  xAiSessionDestroy(sess);
  EXPECT_EQ(cap.calls, 1);
  EXPECT_EQ(cap.seen_sess, sess); /* hook sees its own handle */
  EXPECT_EQ(cap.seen_owner, &cap);
  EXPECT_GT(cap.history_len, 0u); /* history was still intact */
}

/* ── Text-only happy path ───────────────────────────────────────────── */

TEST_F(SessionTest, TextOnlyRoundDeliversTextAndDone) {
  Captured cap;
  auto cbs  = make_cbs(&cap);
  xAiSession sess = make_session(cbs);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("hello "),
      SText("world"),
      SDone(xAiProviderStop_EndTurn),
  });

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
  fake_->script_queue.push_back({SDone(xAiProviderStop_EndTurn)});

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

  fake_->script_queue.push_back({
      SText("round1"),
      SDone(xAiProviderStop_EndTurn),
  });
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("q1")), xErrno_Ok);

  fake_->script_queue.push_back({
      SText("round2"),
      SDone(xAiProviderStop_EndTurn),
  });
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

  fake_->script_queue.push_back({
      SText("partial"),
      SDone(xAiProviderStop_Error, xErrno_Again),
  });
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  /* Contract (see session.h): on_error fires as a diagnostic
   * precursor AND on_done always fires as the authoritative
   * terminator. Every accepted Input() produces exactly one
   * on_done regardless of success/failure. */
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
  fake_->script_queue.push_back({SDone(xAiProviderStop_StopSeq)});
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Stopped);
  xAiSessionDestroy(sess);
}

TEST_F(SessionTest, ProviderPromptLongMapsToPromptTooLong) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));
  fake_->script_queue.push_back({SDone(xAiProviderStop_PromptLong)});
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_PromptTooLong);
  xAiSessionDestroy(sess);
}

/* ── tool_use path: see the "tool loop" section below ──────────────── */

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
static void cb_reenter_done(xAiSession, xAiDoneReason, const xAiUsage *,
                            void *) {}

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

  fake_->script_queue.push_back({
      SText("hello"),
      SDone(xAiProviderStop_EndTurn),
  });
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
  fake_->script_queue.push_back({SText("partial")});
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
  EXPECT_EQ(s->query.running, 0);

  /* And a fresh attempt is allowed. */
  fake_->submit_return = xErrno_Ok;
  fake_->script_queue.push_back({SDone(xAiProviderStop_EndTurn)});
  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("retry")), xErrno_Ok);

  xAiSessionDestroy(sess);
}

/* ── Tool loop ──────────────────────────────────────────────────────── */

/* A tool handler that echoes its args_json back as the result output.
 * Records every invocation in the pointed-to vector for assertions. */
struct ToolRec {
  std::string name;
  std::string id;
  std::string args;
};

static xErrno echo_handler(const xAiContent *in, xAiContent *out, void *ud) {
  auto *log = static_cast<std::vector<ToolRec> *>(ud);
  ToolRec rec;
  rec.name = in->u.tool_use.name ? in->u.tool_use.name : "";
  rec.id   = in->u.tool_use.id   ? in->u.tool_use.id   : "";
  rec.args = in->u.tool_use.args_json ? in->u.tool_use.args_json : "";
  log->push_back(rec);

  /* The handler doc says any pointer written into out must remain
   * valid for the duration of the call; session.c is then
   * responsible for copying it before the handler returns.
   * std::vector's back() pointer is stable inside this function. */
  out->type                      = xAiContentType_ToolResult;
  out->u.tool_result.id          = log->back().id.c_str();
  out->u.tool_result.output      = log->back().args.c_str();
  out->u.tool_result.output_len  = log->back().args.size();
  out->u.tool_result.is_error    = 0;
  return xErrno_Ok;
}

static xErrno failing_handler(const xAiContent *, xAiContent *, void *) {
  return xErrno_Again;
}

/* Helper to spin up a session whose agent carries the given tool(s). */
class ToolLoopFixture : public SessionTest {
 protected:
  xAiTool tool_echo_    = nullptr;
  xAiTool tool_failing_ = nullptr;

  std::vector<ToolRec> echo_log_;

  /* Override SetUp so the agent has tools registered. SessionTest's
   * original agent_ has none, so we tear it down and rebuild one. */
  void SetUp() override {
    SessionTest::SetUp();
    xAiAgentDestroy(agent_);

    xAiToolConf tc = {};
    tc.name        = "echo";
    tc.description = "echo args back";
    tc.json_schema = "{\"type\":\"object\"}";
    tc.handler     = echo_handler;
    tc.user_data   = &echo_log_;
    tool_echo_     = xAiToolCreate(&tc);

    tc           = {};
    tc.name      = "boom";
    tc.handler   = failing_handler;
    tool_failing_ = xAiToolCreate(&tc);

    /* agent borrows this array; keep it alive beyond SetUp() by
     * making it function-static. xAiTool is a typedef for void*, so
     * `const xAiTool *` is really `void *const *` — we hold the
     * address of the handle, not the handle itself. */
    static const xAiTool *kTools[2];
    kTools[0] = &tool_echo_;
    kTools[1] = &tool_failing_;

    xAiAgentConf ac   = {};
    ac.loop           = loop_;
    ac.provider       = provider_;
    ac.model          = "fake-model";
    ac.system_prompt  = "you are a test";
    ac.max_turns      = 5;
    ac.max_tokens     = 1024;
    ac.context_budget = 8192;
    ac.tools          = kTools;
    ac.n_tools        = 2;
    agent_            = xAiAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }

  void TearDown() override {
    xAiToolDestroy(tool_echo_);
    xAiToolDestroy(tool_failing_);
    SessionTest::TearDown();
  }
};

/* Single tool_use → handler → tool_result → model finishes. */
TEST_F(ToolLoopFixture, SingleToolRoundTrip) {
  struct LocalCap : Captured {
    std::vector<std::pair<std::string, int>> tool_events;
  } cap;

  auto cbs        = make_cbs(&cap);
  cbs.on_tool     = [](xAiSession, const char *name, int started, void *ud) {
    auto *c = static_cast<LocalCap *>(ud);
    c->tool_events.push_back({name, started});
  };
  xAiSession sess = make_session(cbs);

  /* Round 1: model thinks, then calls echo. */
  fake_->script_queue.push_back({
      SText("thinking... "),
      SToolCall("echo", "call_42", "{\"hello\":\"world\"}"),
      SDone(xAiProviderStop_ToolUse),
  });
  /* Round 2: model acknowledges the result and ends the turn. */
  fake_->script_queue.push_back({
      SText("done."),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("please echo")),
            xErrno_Ok);

  /* The session drove two provider submits. */
  EXPECT_EQ(fake_->submits, 2);

  /* Handler ran exactly once, saw the right args. */
  ASSERT_EQ(echo_log_.size(), 1u);
  EXPECT_EQ(echo_log_[0].name, "echo");
  EXPECT_EQ(echo_log_[0].id, "call_42");
  EXPECT_EQ(echo_log_[0].args, "{\"hello\":\"world\"}");

  /* on_tool bracketed the handler with started=1 / started=0. */
  ASSERT_EQ(cap.tool_events.size(), 2u);
  EXPECT_EQ(cap.tool_events[0].first, "echo");
  EXPECT_EQ(cap.tool_events[0].second, 1);
  EXPECT_EQ(cap.tool_events[1].second, 0);

  /* Streaming text from both rounds reached the caller in order. */
  EXPECT_EQ(cap.texts, "thinking... done.");

  /* Exactly one final on_done(Completed). */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);
  EXPECT_EQ(cap.error_fired, 0);

  /* History layout:
   *   [0] user "please echo"
   *   [1] assistant text "thinking... "
   *   [2] assistant tool_use (echo, call_42)
   *   [3] tool tool_result (call_42, args echoed back)
   *   [4] assistant text "done." */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  ASSERT_EQ(s->n_history, 5u);
  EXPECT_EQ(s->history[0].role, xAiRole_User);
  EXPECT_EQ(s->history[1].role, xAiRole_Assistant);
  EXPECT_EQ(s->history[1].kind, xAiSessionEntry_Text);
  EXPECT_STREQ(s->history[1].text, "thinking... ");
  EXPECT_EQ(s->history[2].role, xAiRole_Assistant);
  EXPECT_EQ(s->history[2].kind, xAiSessionEntry_ToolUse);
  EXPECT_STREQ(s->history[2].tool_use_name, "echo");
  EXPECT_STREQ(s->history[2].tool_use_id, "call_42");
  EXPECT_EQ(s->history[3].role, xAiRole_Tool);
  EXPECT_EQ(s->history[3].kind, xAiSessionEntry_ToolResult);
  EXPECT_STREQ(s->history[3].tool_result_id, "call_42");
  EXPECT_EQ(std::string(s->history[3].tool_result_output,
                         s->history[3].tool_result_output_len),
            "{\"hello\":\"world\"}");
  EXPECT_EQ(s->history[3].tool_result_is_error, 0);
  EXPECT_EQ(s->history[4].role, xAiRole_Assistant);
  EXPECT_STREQ(s->history[4].text, "done.");

  /* The second submit should have carried the full assistant turn
   * (text + tool_use blocks folded into ONE message) plus the
   * tool_result message. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &msgs2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(text+tool_use), tool] */
  ASSERT_EQ(msgs2.size(), 4u);
  EXPECT_EQ(msgs2[0].role, xAiRole_System);
  EXPECT_EQ(msgs2[1].role, xAiRole_User);
  EXPECT_EQ(msgs2[2].role, xAiRole_Assistant);
  /* Folded: assistant carries two blocks — text, then tool_use. */
  ASSERT_EQ(msgs2[2].blocks.size(), 2u);
  EXPECT_EQ(msgs2[2].blocks[0].type, xAiContentType_Text);
  EXPECT_EQ(msgs2[2].blocks[0].text, "thinking... ");
  EXPECT_EQ(msgs2[2].blocks[1].type, xAiContentType_ToolUse);
  EXPECT_EQ(msgs2[2].blocks[1].tool_use_name, "echo");
  EXPECT_EQ(msgs2[2].blocks[1].tool_use_id, "call_42");
  /* Tool role carries the result. */
  EXPECT_EQ(msgs2[3].role, xAiRole_Tool);
  ASSERT_EQ(msgs2[3].blocks.size(), 1u);
  EXPECT_EQ(msgs2[3].blocks[0].type, xAiContentType_ToolResult);
  EXPECT_EQ(msgs2[3].blocks[0].tool_use_id, "call_42");
  EXPECT_EQ(msgs2[3].blocks[0].tool_result_output, "{\"hello\":\"world\"}");

  xAiSessionDestroy(sess);
}

/* Two parallel tool_calls in one assistant turn, both dispatched. */
TEST_F(ToolLoopFixture, MultipleToolCallsInOneTurn) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("echo", "c1", "{\"x\":1}"),
      SToolCall("echo", "c2", "{\"x\":2}"),
      SDone(xAiProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAiProviderStop_EndTurn)});

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("go")), xErrno_Ok);

  /* Handler ran twice, in order. */
  ASSERT_EQ(echo_log_.size(), 2u);
  EXPECT_EQ(echo_log_[0].id, "c1");
  EXPECT_EQ(echo_log_[1].id, "c2");

  /* Completed, no error. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);

  /* Second submit: assistant turn folded two tool_use blocks,
   * followed by two tool_result messages. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(2 tool_uses), tool(c1), tool(c2)] */
  ASSERT_EQ(m2.size(), 5u);
  EXPECT_EQ(m2[2].role, xAiRole_Assistant);
  ASSERT_EQ(m2[2].blocks.size(), 2u);
  EXPECT_EQ(m2[2].blocks[0].type, xAiContentType_ToolUse);
  EXPECT_EQ(m2[2].blocks[0].tool_use_id, "c1");
  EXPECT_EQ(m2[2].blocks[1].tool_use_id, "c2");
  EXPECT_EQ(m2[3].role, xAiRole_Tool);
  EXPECT_EQ(m2[3].blocks[0].tool_use_id, "c1");
  EXPECT_EQ(m2[4].role, xAiRole_Tool);
  EXPECT_EQ(m2[4].blocks[0].tool_use_id, "c2");

  xAiSessionDestroy(sess);
}

/* Unknown tool: session fabricates an error tool_result and keeps
 * looping — the MODEL gets to react, not the caller. */
TEST_F(ToolLoopFixture, UnknownToolFeedsErrorBackToModel) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("no_such_tool", "c9", "{}"),
      SDone(xAiProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("ok I'll stop"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("use weird tool")),
            xErrno_Ok);

  /* No handler ran. */
  EXPECT_EQ(echo_log_.size(), 0u);

  /* The run completed normally (not ToolError) — the model decides. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);

  /* Second submit carried a tool_result marked is_error=1 with a
   * diagnostic mentioning the tool name. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  bool found_err = false;
  for (const auto &m : m2) {
    if (m.role != xAiRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type != xAiContentType_ToolResult) continue;
      if (b.tool_result_is_error &&
          b.tool_result_output.find("no_such_tool") != std::string::npos) {
        found_err = true;
      }
    }
  }
  EXPECT_TRUE(found_err);

  xAiSessionDestroy(sess);
}

/* Handler returning xErrno_Again yields an error tool_result; loop
 * continues rather than terminating the whole run. */
TEST_F(ToolLoopFixture, HandlerErrorFeedsBackToModel) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("boom", "cb", "{}"),
      SDone(xAiProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("guess I'll quit"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("try")), xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);

  /* Check the tool_result was flagged as an error and mentions Again. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  bool found_err = false;
  for (const auto &m : m2) {
    if (m.role != xAiRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type == xAiContentType_ToolResult && b.tool_result_is_error) {
        found_err = true;
      }
    }
  }
  EXPECT_TRUE(found_err);

  xAiSessionDestroy(sess);
}

/* A runaway tool loop: every round returns ToolUse. The session
 * must stop after max_turns rounds with xAiDoneReason_MaxTurns. */
TEST_F(ToolLoopFixture, MaxTurnsCapsRunawayToolLoop) {
  Captured cap;
  xAiSessionConf sc = {};
  sc.cbs            = make_cbs(&cap);
  sc.max_turns      = 3; /* override the agent's 5 */
  xAiSession sess   = xAiSessionCreate(agent_, &sc);

  /* Push 5 identical scripts; max_turns=3 should stop us before
   * the fourth submit. */
  for (int i = 0; i < 5; i++) {
    fake_->script_queue.push_back({
        SToolCall("echo", "cx", "{}"),
        SDone(xAiProviderStop_ToolUse),
    });
  }

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("loop")), xErrno_Ok);

  /* We accept either 3 submits (cap applied before the 4th submit)
   * — the important contract is "no unbounded looping, correct
   *   on_done reason". */
  EXPECT_EQ(fake_->submits, 3);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_MaxTurns);

  xAiSessionDestroy(sess);
}

/* Default max_turns kicks in when neither conf nor agent set one. */
TEST_F(ToolLoopFixture, DefaultMaxTurnsAppliesWhenUnset) {
  /* Build a fresh agent with max_turns=0 and no override. */
  xAiAgentDestroy(agent_);
  static const xAiTool *kTools[1] = {&tool_echo_};
  xAiAgentConf ac   = {};
  ac.loop           = loop_;
  ac.provider       = provider_;
  ac.model          = "fake-model";
  ac.system_prompt  = "sp";
  ac.tools          = kTools;
  ac.n_tools        = 1;
  agent_            = xAiAgentCreate(&ac);

  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  /* Push way more scripts than the default cap (16) so we can see
   * the cap enforced. */
  for (int i = 0; i < 20; i++) {
    fake_->script_queue.push_back({
        SToolCall("echo", "cx", "{}"),
        SDone(xAiProviderStop_ToolUse),
    });
  }

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("loop")), xErrno_Ok);
  EXPECT_EQ(fake_->submits, 16);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_MaxTurns);

  xAiSessionDestroy(sess);
}

/* Provider reports ToolUse but didn't actually emit any tool_call.
 * Treat as ModelError-style ToolError rather than silently looping. */
TEST_F(ToolLoopFixture, ToolUseWithoutAnyCallsYieldsToolError) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({SDone(xAiProviderStop_ToolUse)});

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("try")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_ToolError);
  EXPECT_EQ(fake_->submits, 1); /* no follow-up */

  xAiSessionDestroy(sess);
}

/* Regression for the "reasoning_content missing in assistant tool call
 * message" error on kimi-k2.6 / DeepSeek-R1 / o1. When the provider
 * streams reasoning deltas alongside a tool_call, the session must
 * stash them and echo the reasoning back inside the same assistant
 * turn on the next submit — otherwise moonshot rejects the follow-up
 * with a 400. */
TEST_F(ToolLoopFixture, AssistantThinkingEchoedInFollowUpRound) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  /* Round 1: reasoning chunks split across deltas (as they arrive on
   * the wire), then a tool call, then finish_reason=tool_calls. */
  fake_->script_queue.push_back({
      SThinking("I should "),
      SThinking("call echo."),
      SToolCall("echo", "call_7", "{\"x\":1}"),
      SDone(xAiProviderStop_ToolUse),
  });
  /* Round 2: model acknowledges and ends the turn. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  /* History must now contain a dedicated Thinking entry before the
   * ToolUse entry inside the assistant turn, so view_build can emit
   * the reasoning_content block on round 2. */
  auto *s = reinterpret_cast<xAiSession_ *>(sess);
  /* [0] user, [1] assistant thinking, [2] assistant tool_use,
   * [3] tool result, [4] assistant text "ok" */
  ASSERT_EQ(s->n_history, 5u);
  EXPECT_EQ(s->history[1].role, xAiRole_Assistant);
  EXPECT_EQ(s->history[1].kind, xAiSessionEntry_Thinking);
  EXPECT_STREQ(s->history[1].text, "I should call echo.");
  EXPECT_EQ(s->history[2].kind, xAiSessionEntry_ToolUse);

  /* Second submit: the assistant message carries thinking + tool_use
   * as distinct content blocks. Provider serialisers (OpenAI-compat)
   * rely on seeing the Thinking block to emit the reasoning_content
   * field. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &msgs2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(thinking+tool_use), tool] */
  ASSERT_EQ(msgs2.size(), 4u);
  EXPECT_EQ(msgs2[2].role, xAiRole_Assistant);
  ASSERT_EQ(msgs2[2].blocks.size(), 2u);
  EXPECT_EQ(msgs2[2].blocks[0].type, xAiContentType_Thinking);
  EXPECT_EQ(msgs2[2].blocks[0].text, "I should call echo.");
  EXPECT_EQ(msgs2[2].blocks[1].type, xAiContentType_ToolUse);

  xAiSessionDestroy(sess);
}

/* Usage numbers from each provider round must fold together so the
 * caller sees cumulative input/output tokens for the entire run, not
 * just the last round. This is what the REPL / xbuddy surfaces in
 * "tokens=179/88 total=267" style lines. */
TEST_F(ToolLoopFixture, UsageAccumulatesAcrossToolLoop) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  /* Round 1: 100 prompt / 20 completion / 120 total. Model calls echo. */
  fake_->script_queue.push_back({
      SToolCall("echo", "call_u1", "{}"),
      SDoneWithUsage(xAiProviderStop_ToolUse, 100, 20, 120),
  });
  /* Round 2: 150 prompt / 30 completion / 180 total. Model finishes. */
  fake_->script_queue.push_back({
      SText("done"),
      SDoneWithUsage(xAiProviderStop_EndTurn, 150, 30, 180),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(fake_->submits, 2);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAiDoneReason_Completed);

  /* Cumulative totals — not just the last round. */
  ASSERT_TRUE(cap.has_usage);
  EXPECT_EQ(cap.usage.prompt_tokens, 250);
  EXPECT_EQ(cap.usage.completion_tokens, 50);
  EXPECT_EQ(cap.usage.total_tokens, 300);

  xAiSessionDestroy(sess);
}

/* A round that reports NULL usage (server stayed silent) must not
 * poison the accumulator — we carry forward whatever earlier rounds
 * reported. Mirrors claude-code's behaviour of skipping over missing
 * usage snapshots rather than zeroing the totals. */
TEST_F(ToolLoopFixture, UsageSurvivesRoundWithoutUsage) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  /* Round 1: reports numbers. */
  fake_->script_queue.push_back({
      SToolCall("echo", "call_u2", "{}"),
      SDoneWithUsage(xAiProviderStop_ToolUse, 42, 7, 49),
  });
  /* Round 2: deliberately no usage (SDone, not SDoneWithUsage). */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  /* Round-1 numbers must survive — missing round neither adds nor
   * resets. */
  ASSERT_TRUE(cap.has_usage);
  EXPECT_EQ(cap.usage.prompt_tokens, 42);
  EXPECT_EQ(cap.usage.completion_tokens, 7);
  EXPECT_EQ(cap.usage.total_tokens, 49);

  xAiSessionDestroy(sess);
}

/* When every round is silent about usage the caller gets a NULL
 * pointer in on_done — not a bogus 0/0/0 "free run" reading. */
TEST_F(SessionTest, UsageStaysNullWhenProviderNeverReports) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SText("hi"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  /* No round reported anything → session hands NULL to the caller. */
  EXPECT_FALSE(cap.has_usage);

  xAiSessionDestroy(sess);
}

/* Reasoning-capable models (kimi-k2.6 thinking, DeepSeek-R1, o1, ...)
 * stream their chain-of-thought on a separate channel. The session
 * layer MUST forward those deltas to cbs.on_thinking in order,
 * strictly before any text, and it MUST NOT leak reasoning bytes
 * into cbs.on_text — the REPL renders them very differently (dim +
 * [thinking] prefix) and mixing the two would be a correctness
 * disaster. */
TEST_F(SessionTest, StreamsThinkingToCaller) {
  Captured cap;
  xAiSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SThinking("I should "),
      SThinking("say hi."),
      SText("hello"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);

  /* Thinking deltas reassembled in arrival order. */
  EXPECT_EQ(cap.thinking, "I should say hi.");
  EXPECT_EQ(cap.thinking_fired, 2);

  /* Text channel stayed clean — no reasoning bleed. */
  EXPECT_EQ(cap.texts, "hello");

  xAiSessionDestroy(sess);
}

/* Callers that don't care about reasoning leave on_thinking NULL.
 * The session must still accept the deltas (some servers, notably
 * kimi-k2.6, require them to be echoed back on the next tool-loop
 * round or they'll 400) — it just shouldn't crash or spill them
 * into on_text. */
TEST_F(SessionTest, ThinkingWithoutCallbackDoesNotCrash) {
  Captured cap;
  xAiSessionCallbacks cbs = make_cbs(&cap);
  cbs.on_thinking = nullptr; /* opt out */
  xAiSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SThinking("hidden reasoning"),
      SText("visible"),
      SDone(xAiProviderStop_EndTurn),
  });

  EXPECT_EQ(xAiSessionInput(sess, xAiMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  /* Thinking callback wasn't set → nothing captured there. */
  EXPECT_EQ(cap.thinking, "");
  EXPECT_EQ(cap.thinking_fired, 0);
  /* And it didn't bleed into on_text. */
  EXPECT_EQ(cap.texts, "visible");

  xAiSessionDestroy(sess);
}


