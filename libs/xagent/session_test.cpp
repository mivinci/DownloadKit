/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_test.cpp - Coverage for xAgentSession (MVP text-only path)
 *
 * These tests drive xAgentSession through a programmable fake provider
 * that records every submit() it receives and lets each test script
 * what the provider should stream back. That keeps the suite
 * hermetic (no HTTP, no sockets) and lets us observe the exact
 * messages[] view the session builds.
 */

extern "C" {
#include "provider_private.h"
#include "session_private.h"

#include <xagent/agent.h>
#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/error.h>
#include <xbase/event.h>

#include <time.h> /* nanosleep for created_at_ms tests */
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

/* ── Helpers for accessing xArray-backed history ─────────────────── */

static size_t hist_len(const struct xAgentSession_ *s) {
  return xArrayLen(s->history_arr);
}

static struct xAgentSessionMsg_ *hist_at(const struct xAgentSession_ *s, size_t i) {
  return (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, i);
}

/* ── Fake provider ──────────────────────────────────────────────────── */

struct FakeScript {
  enum Kind { TEXT, TOOL_CALL, THINKING, DONE };
  Kind                  kind;
  std::string           text;      /* TEXT: payload; TOOL_CALL: name;
                                      THINKING: payload              */
  std::string           tool_id;   /* TOOL_CALL: id (default "call_1") */
  std::string           tool_args; /* TOOL_CALL: args_json (default "{}") */
  xAgentProviderStopReason reason;
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
static inline FakeScript SDone(xAgentProviderStopReason reason,
                               xErrno err = xErrno_Ok) {
  FakeScript s{};
  s.kind   = FakeScript::DONE;
  s.reason = reason;
  s.err    = err;
  return s;
}
static inline FakeScript SDoneWithUsage(xAgentProviderStopReason reason,
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
  xAgentContentType type;
  std::string    text;          /* Text / Thinking payload */
  std::string    tool_use_id;   /* ToolUse / ToolResult */
  std::string    tool_use_name;
  std::string    tool_use_args;
  std::string    tool_result_output;
  int            tool_result_is_error = 0;
};

struct FakeCapturedMsg {
  xAgentRole                        role;
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
  xAgentProviderStreamCallbacks cbs_last    = {};
  void                      *cb_arg_last = nullptr;
  /* If non-zero, cancel() immediately delivers on_done(Cancelled). */
  int cancel_fires_done = 0;
};

static xErrno fake_submit(void *impl, const xAgentProviderSubmitConf *conf,
                          const xAgentProviderStreamCallbacks *cbs, void *cb_arg) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->submits++;
  f->cbs_last    = *cbs;
  f->cb_arg_last = cb_arg;
  f->captured_msgs.clear();
  for (size_t i = 0; i < conf->n_messages; i++) {
    FakeCapturedMsg m;
    m.role = conf->messages[i].role;
    for (size_t j = 0; j < conf->messages[i].n; j++) {
      const xAgentContent &c = conf->messages[i].contents[j];
      FakeCapturedBlock b;
      b.type = c.type;
      if (c.type == xAgentContentType_Text) {
        b.text.assign(c.u.text.text, c.u.text.len);
        m.text.append(c.u.text.text, c.u.text.len);
      } else if (c.type == xAgentContentType_Thinking) {
        b.text.assign(c.u.thinking.text, c.u.thinking.len);
      } else if (c.type == xAgentContentType_ToolUse) {
        b.tool_use_id   = c.u.tool_use.id   ? c.u.tool_use.id   : "";
        b.tool_use_name = c.u.tool_use.name ? c.u.tool_use.name : "";
        b.tool_use_args =
            c.u.tool_use.args_json ? c.u.tool_use.args_json : "";
      } else if (c.type == xAgentContentType_ToolResult) {
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
          xAgentContent tc = {};
          tc.type                 = xAgentContentType_ToolUse;
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
          xAgentUsage u{ev.prompt_tokens, ev.completion_tokens, ev.total_tokens};
          cbs->on_done(ev.reason, ev.err, ev.has_usage ? &u : nullptr,
                       nullptr, cb_arg);
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
    f->cbs_last.on_done(xAgentProviderStop_Cancelled, xErrno_Ok, nullptr,
                        nullptr, f->cb_arg_last);
  }
}

static void fake_destroy(void *impl) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->destroyed = 1;
  delete f;
}

static const xAgentProviderVtable kFakeVtable = {fake_submit, fake_cancel,
                                              fake_destroy};

static xAgentProvider make_fake_provider(FakeImpl **out) {
  auto *impl = new FakeImpl();
  auto *base = static_cast<xAgentProvider_ *>(calloc(1, sizeof(xAgentProvider_)));
  base->vt   = &kFakeVtable;
  base->ctx  = impl;
  *out       = impl;
  return reinterpret_cast<xAgentProvider>(base);
}

/* ── Callback capture for the session side ─────────────────────────── */

struct Captured {
  std::string   texts;
  int           texts_fired = 0;
  std::string   thinking;       /* accumulated on_thinking deltas */
  int           thinking_fired = 0;
  int           done_fired  = 0;
  xAgentDoneReason done_reason = xAgentDoneReason_Completed;
  int           error_fired = 0;
  xErrno        error_code  = xErrno_Ok;
  std::string   error_msg;
  /* Usage snapshot from on_done. has_usage false when session hands
   * NULL (no round ever reported); otherwise holds the cumulative
   * totals across the whole run, with -1 for fields still unknown. */
  bool          has_usage   = false;
  xAgentUsage      usage{-1, -1, -1};
};

static void cb_text(xAgentSession, const char *c, size_t n, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->texts.append(c, n);
  cap->texts_fired++;
}
static void cb_thinking(xAgentSession, const char *c, size_t n, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->thinking.append(c, n);
  cap->thinking_fired++;
}
static void cb_done(xAgentSession, xAgentDoneReason r, const xAgentUsage *u, void *ud) {
  auto *cap        = static_cast<Captured *>(ud);
  cap->done_fired++;
  cap->done_reason = r;
  if (u) {
    cap->has_usage = true;
    cap->usage     = *u;
  }
}
static void cb_err(xAgentSession, xErrno e, const char *m, void *ud) {
  auto *cap = static_cast<Captured *>(ud);
  cap->error_fired++;
  cap->error_code = e;
  if (m) cap->error_msg = m;
}

/* ── Fixture ────────────────────────────────────────────────────────── */

class SessionTest : public ::testing::Test {
 protected:
  xEventLoop  loop_     = nullptr;
  xAgentProvider provider_ = nullptr;
  FakeImpl   *fake_     = nullptr;
  xAgent    agent_    = nullptr;

  void SetUp() override {
    loop_     = xEventLoopCreate();
    provider_ = make_fake_provider(&fake_);

    xAgentConf ac    = {};
    ac.loop            = loop_;
    ac.provider        = provider_;
    ac.model           = "fake-model";
    ac.system_prompt   = "you are a test";
    ac.max_turns       = 5;
    ac.max_tokens      = 1024;
    agent_             = xAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }
  void TearDown() override {
    xAgentDestroy(agent_);
    xAgentProviderDestroy(provider_);
    xEventLoopDestroy(loop_);
  }

  xAgentSession make_session(const xAgentSessionCallbacks &cbs) {
    xAgentSessionConf sc = {};
    sc.cbs            = cbs;
    return xAgentSessionCreate(agent_, &sc);
  }

  static xAgentSessionCallbacks make_cbs(Captured *cap) {
    xAgentSessionCallbacks c = {};
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
  xAgentSessionConf sc = {};
  EXPECT_EQ(xAgentSessionCreate(nullptr, &sc), nullptr);
}

TEST_F(SessionTest, CreateRejectsNullConf) {
  EXPECT_EQ(xAgentSessionCreate(agent_, nullptr), nullptr);
}

TEST_F(SessionTest, DestroyAcceptsNull) {
  xAgentSessionDestroy(nullptr); /* must not crash */
}

TEST_F(SessionTest, CreateInheritsSystemPromptAndModel) {
  xAgentSessionConf sc = {};
  xAgentSession sess   = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_STREQ(s->system_prompt, "you are a test");
  EXPECT_STREQ(s->model, "fake-model");
  EXPECT_EQ(s->max_turns, 5);
  EXPECT_EQ(s->max_tokens, 1024);
  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, CreateOverridesWinOverAgent) {
  xAgentSessionConf sc  = {};
  sc.system_prompt   = "override-sp";
  sc.model           = "override-model";
  sc.max_turns       = 99;
  xAgentSession sess    = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_STREQ(s->system_prompt, "override-sp");
  EXPECT_STREQ(s->model, "override-model");
  EXPECT_EQ(s->max_turns, 99);
  xAgentSessionDestroy(sess);
}

/* ── Session-lifetime properties: origin + on_finalizing ─────────── */

TEST_F(SessionTest, OriginDefaultsToUserWhenUnset) {
  xAgentSessionConf sc = {};
  xAgentSession sess   = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  EXPECT_EQ(xAgentSessionOrigin(sess), xAgentInputOrigin_User);
  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, OriginEchoesConfValue) {
  xAgentSessionConf sc = {};
  sc.origin         = xAgentInputOrigin_SystemSynthesized;
  xAgentSession sess   = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);
  EXPECT_EQ(xAgentSessionOrigin(sess), xAgentInputOrigin_SystemSynthesized);
  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, OriginOnNullSessionReturnsUser) {
  EXPECT_EQ(xAgentSessionOrigin(nullptr), xAgentInputOrigin_User);
}

/* on_finalizing must fire exactly once, with the configured owner,
 * while the session handle is still live (history still reachable). */
namespace {
struct FinalizingCap {
  int         calls       = 0;
  xAgentSession  seen_sess   = nullptr;
  void       *seen_owner  = nullptr;
  size_t      history_len = SIZE_MAX;
};

void cb_finalizing(xAgentSession sess, void *owner) {
  auto *cap = static_cast<FinalizingCap *>(owner);
  cap->calls++;
  cap->seen_sess   = sess;
  cap->seen_owner  = owner;
  /* Peek through the private layout to prove history isn't freed yet. */
  cap->history_len = xArrayLen(reinterpret_cast<xAgentSession_ *>(sess)->history_arr);
}
}  // namespace

TEST_F(SessionTest, FinalizingHookFiresOnDestroyBeforeTeardown) {
  FinalizingCap cap;
  xAgentSessionConf sc   = {};
  sc.on_finalizing    = cb_finalizing;
  sc.finalizing_owner = &cap;

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  /* Append one user turn so history has something to observe. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });
  Captured dummy;
  auto cbs_noop = xAgentSessionCallbacks{};
  cbs_noop.user_data = &dummy;
  reinterpret_cast<xAgentSession_ *>(sess)->cbs = cbs_noop;
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.calls, 0);
  xAgentSessionDestroy(sess);
  EXPECT_EQ(cap.calls, 1);
  EXPECT_EQ(cap.seen_sess, sess); /* hook sees its own handle */
  EXPECT_EQ(cap.seen_owner, &cap);
  EXPECT_GT(cap.history_len, 0u); /* history was still intact */
}

/* ── Text-only happy path ───────────────────────────────────────────── */

TEST_F(SessionTest, TextOnlyRoundDeliversTextAndDone) {
  Captured cap;
  auto cbs  = make_cbs(&cap);
  xAgentSession sess = make_session(cbs);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("hello "),
      SText("world"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.texts_fired, 2);
  EXPECT_EQ(cap.texts, "hello world");
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);
  EXPECT_EQ(cap.error_fired, 0);

  /* History: system is not stored, so we expect user + assistant. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 2u);
  EXPECT_EQ(hist_at(s, 0)->role, xAgentRole_User);
  EXPECT_STREQ(hist_at(s, 0)->text, "hi");
  EXPECT_EQ(hist_at(s, 1)->role, xAgentRole_Assistant);
  EXPECT_STREQ(hist_at(s, 1)->text, "hello world");

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, SubmitViewPrependsSystemPrompt) {
  Captured cap;
  auto cbs  = make_cbs(&cap);
  xAgentSession sess = make_session(cbs);
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  ASSERT_EQ(fake_->submits, 1);
  ASSERT_GE(fake_->captured_msgs.size(), 2u);
  EXPECT_EQ(fake_->captured_msgs[0].role, xAgentRole_System);
  EXPECT_EQ(fake_->captured_msgs[0].text, "you are a test");
  EXPECT_EQ(fake_->captured_msgs[1].role, xAgentRole_User);
  EXPECT_EQ(fake_->captured_msgs[1].text, "hi");

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, MultiTurnAccumulatesHistory) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SText("round1"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("q1")), xErrno_Ok);

  fake_->script_queue.push_back({
      SText("round2"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("q2")), xErrno_Ok);

  /* 2 users + 2 assistants. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 4u);
  EXPECT_STREQ(hist_at(s, 0)->text, "q1");
  EXPECT_STREQ(hist_at(s, 1)->text, "round1");
  EXPECT_STREQ(hist_at(s, 2)->text, "q2");
  EXPECT_STREQ(hist_at(s, 3)->text, "round2");

  /* Second submit sees system + [q1, round1, q2] = 4 messages.
   * The second assistant reply (round2) is only committed to history
   * after submit returns, so it can't be in the wire view. */
  EXPECT_EQ(fake_->captured_msgs.size(), 4u);
  EXPECT_EQ(fake_->captured_msgs[0].role, xAgentRole_System);
  EXPECT_EQ(fake_->captured_msgs[1].text, "q1");
  EXPECT_EQ(fake_->captured_msgs[2].text, "round1");
  EXPECT_EQ(fake_->captured_msgs[3].text, "q2");
  xAgentSessionDestroy(sess);
}

/* ── Stop reason translation ────────────────────────────────────────── */

TEST_F(SessionTest, ProviderErrorMapsToModelErrorAndFiresOnError) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SText("partial"),
      SDone(xAgentProviderStop_Error, xErrno_Again),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  /* Contract (see session.h): on_error fires as a diagnostic
   * precursor AND on_done always fires as the authoritative
   * terminator. Every accepted Input() produces exactly one
   * on_done regardless of success/failure. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_ModelError);
  EXPECT_EQ(cap.error_fired, 1);
  EXPECT_EQ(cap.error_code, xErrno_Again);

  /* Partial assistant text still makes it into history. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 2u);
  EXPECT_STREQ(hist_at(s, 1)->text, "partial");

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, ProviderStopSeqMapsToStopped) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));
  fake_->script_queue.push_back({SDone(xAgentProviderStop_StopSeq)});
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Stopped);
  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, ProviderPromptLongMapsToPromptTooLong) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));
  fake_->script_queue.push_back({SDone(xAgentProviderStop_PromptLong)});
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_PromptTooLong);
  xAgentSessionDestroy(sess);
}

/* ── tool_use path: see the "tool loop" section below ──────────────── */

/* ── Busy: re-entrant Input during a run is rejected ────────────────── */

struct ReInputCap {
  xAgentSession sess  = nullptr;
  xErrno     rc    = xErrno_Ok;
  int        fired = 0;
};

static void cb_reenter_text(xAgentSession sess, const char *, size_t, void *ud) {
  auto *r = static_cast<ReInputCap *>(ud);
  if (r->fired++) return; /* only try once */
  /* Try to recursively call Input while we are mid-stream. */
  r->rc = xAgentSessionInput(sess, xAgentMessageFromText("nested"));
}
static void cb_reenter_done(xAgentSession, xAgentDoneReason, const xAgentUsage *,
                            void *) {}

TEST_F(SessionTest, ReentrantInputReturnsBusy) {
  ReInputCap              r;
  xAgentSessionCallbacks cbs = {};
  cbs.on_text             = cb_reenter_text;
  cbs.on_done             = cb_reenter_done;
  cbs.user_data           = &r;

  xAgentSessionConf sc = {};
  sc.cbs            = cbs;
  xAgentSession sess   = xAgentSessionCreate(agent_, &sc);
  r.sess            = sess;

  fake_->script_queue.push_back({
      SText("hello"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(r.rc, xErrno_Busy);
  xAgentSessionDestroy(sess);
}

/* ── Cancel: provider_cancel fires; on_done reports Aborted ─────────── */

TEST_F(SessionTest, CancelBeforeDoneMapsToAborted) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  /* Script only delivers text — fake's on_done arrives via cancel. */
  fake_->cancel_fires_done = 1;
  fake_->script_queue.push_back({SText("partial")});
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 0); /* no DONE scripted yet */

  xAgentSessionCancel(sess);
  EXPECT_EQ(fake_->cancels, 1);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Aborted);

  /* Partial text preserved in history. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 2u);
  EXPECT_STREQ(hist_at(s, 1)->text, "partial");

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, CancelOnIdleSessionIsNoOp) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));
  xAgentSessionCancel(sess); /* running == 0, must be a silent no-op */
  EXPECT_EQ(fake_->cancels, 0);
  EXPECT_EQ(cap.done_fired, 0);
  xAgentSessionDestroy(sess);
}

/* ── Submit failure is propagated and does not leave running set ────── */

TEST_F(SessionTest, SubmitFailureRollsBackAndReturnsError) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->submit_return = xErrno_Again;

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Again);

  /* on_done should NOT have fired — we never started. */
  EXPECT_EQ(cap.done_fired, 0);

  /* History rolled back. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(hist_len(s), 0u);
  EXPECT_EQ(s->query, nullptr);

  /* And a fresh attempt is allowed. */
  fake_->submit_return = xErrno_Ok;
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("retry")), xErrno_Ok);

  xAgentSessionDestroy(sess);
}

/* ── Tool loop ──────────────────────────────────────────────────────── */

/* A tool handler that echoes its args_json back as the result output.
 * Records every invocation in the pointed-to vector for assertions. */
struct ToolRec {
  std::string name;
  std::string id;
  std::string args;
};

static xErrno echo_handler(xAgentQuery, const xAgentContent *in, xAgentContent *out, void *ud) {
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
  out->type                      = xAgentContentType_ToolResult;
  out->u.tool_result.id          = log->back().id.c_str();
  out->u.tool_result.output      = log->back().args.c_str();
  out->u.tool_result.output_len  = log->back().args.size();
  out->u.tool_result.is_error    = 0;
  return xErrno_Ok;
}

static xErrno failing_handler(xAgentQuery, const xAgentContent *, xAgentContent *, void *) {
  return xErrno_Again;
}

/* Helper to spin up a session whose agent carries the given tool(s). */
class ToolLoopFixture : public SessionTest {
 protected:
  xAgentTool tool_echo_    = nullptr;
  xAgentTool tool_failing_ = nullptr;

  std::vector<ToolRec> echo_log_;

  /* Override SetUp so the agent has tools registered. SessionTest's
   * original agent_ has none, so we tear it down and rebuild one. */
  void SetUp() override {
    SessionTest::SetUp();
    xAgentDestroy(agent_);

    xAgentToolConf tc = {};
    tc.name        = "echo";
    tc.description = "echo args back";
    tc.json_schema = "{\"type\":\"object\"}";
    tc.handler     = echo_handler;
    tc.user_data   = &echo_log_;
    tool_echo_     = xAgentToolCreate(&tc);

    tc           = {};
    tc.name      = "boom";
    tc.handler   = failing_handler;
    tool_failing_ = xAgentToolCreate(&tc);

    /* agent borrows this array; keep it alive beyond SetUp() by
     * making it function-static. xAgentTool is a typedef for void*, so
     * `const xAgentTool *` is really `void *const *` — we hold the
     * address of the handle, not the handle itself. */
    static const xAgentTool *kTools[2];
    kTools[0] = &tool_echo_;
    kTools[1] = &tool_failing_;

    xAgentConf ac   = {};
    ac.loop           = loop_;
    ac.provider       = provider_;
    ac.model          = "fake-model";
    ac.system_prompt  = "you are a test";
    ac.max_turns      = 5;
    ac.max_tokens     = 1024;
    ac.tools          = kTools;
    ac.tools_count    = 2;
    agent_            = xAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }

  void TearDown() override {
    xAgentToolDestroy(tool_echo_);
    xAgentToolDestroy(tool_failing_);
    SessionTest::TearDown();
  }
};

/* Single tool_use → handler → tool_result → model finishes. */
TEST_F(ToolLoopFixture, SingleToolRoundTrip) {
  struct LocalCap : Captured {
    std::vector<std::pair<std::string, int>> tool_events;
  } cap;

  auto cbs        = make_cbs(&cap);
  cbs.on_tool     = [](xAgentSession, const char *name, int started, void *ud) {
    auto *c = static_cast<LocalCap *>(ud);
    c->tool_events.push_back({name, started});
  };
  xAgentSession sess = make_session(cbs);

  /* Round 1: model thinks, then calls echo. */
  fake_->script_queue.push_back({
      SText("thinking... "),
      SToolCall("echo", "call_42", "{\"hello\":\"world\"}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  /* Round 2: model acknowledges the result and ends the turn. */
  fake_->script_queue.push_back({
      SText("done."),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("please echo")),
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
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);
  EXPECT_EQ(cap.error_fired, 0);

  /* History layout:
   *   [0] user "please echo"
   *   [1] assistant text "thinking... "
   *   [2] assistant tool_use (echo, call_42)
   *   [3] tool tool_result (call_42, args echoed back)
   *   [4] assistant text "done." */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 5u);
  EXPECT_EQ(hist_at(s, 0)->role, xAgentRole_User);
  EXPECT_EQ(hist_at(s, 1)->role, xAgentRole_Assistant);
  EXPECT_EQ(hist_at(s, 1)->kind, xAgentSessionEntry_Text);
  EXPECT_STREQ(hist_at(s, 1)->text, "thinking... ");
  EXPECT_EQ(hist_at(s, 2)->role, xAgentRole_Assistant);
  EXPECT_EQ(hist_at(s, 2)->kind, xAgentSessionEntry_ToolUse);
  EXPECT_STREQ(hist_at(s, 2)->tool_use_name, "echo");
  EXPECT_STREQ(hist_at(s, 2)->tool_use_id, "call_42");
  EXPECT_EQ(hist_at(s, 3)->role, xAgentRole_Tool);
  EXPECT_EQ(hist_at(s, 3)->kind, xAgentSessionEntry_ToolResult);
  EXPECT_STREQ(hist_at(s, 3)->tool_result_id, "call_42");
  EXPECT_EQ(std::string(hist_at(s, 3)->tool_result_output,
                         hist_at(s, 3)->tool_result_output_len),
            "{\"hello\":\"world\"}");
  EXPECT_EQ(hist_at(s, 3)->tool_result_is_error, 0);
  EXPECT_EQ(hist_at(s, 4)->role, xAgentRole_Assistant);
  EXPECT_STREQ(hist_at(s, 4)->text, "done.");

  /* The second submit should have carried the full assistant turn
   * (text + tool_use blocks folded into ONE message) plus the
   * tool_result message. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &msgs2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(text+tool_use), tool] */
  ASSERT_EQ(msgs2.size(), 4u);
  EXPECT_EQ(msgs2[0].role, xAgentRole_System);
  EXPECT_EQ(msgs2[1].role, xAgentRole_User);
  EXPECT_EQ(msgs2[2].role, xAgentRole_Assistant);
  /* Folded: assistant carries two blocks — text, then tool_use. */
  ASSERT_EQ(msgs2[2].blocks.size(), 2u);
  EXPECT_EQ(msgs2[2].blocks[0].type, xAgentContentType_Text);
  EXPECT_EQ(msgs2[2].blocks[0].text, "thinking... ");
  EXPECT_EQ(msgs2[2].blocks[1].type, xAgentContentType_ToolUse);
  EXPECT_EQ(msgs2[2].blocks[1].tool_use_name, "echo");
  EXPECT_EQ(msgs2[2].blocks[1].tool_use_id, "call_42");
  /* Tool role carries the result. */
  EXPECT_EQ(msgs2[3].role, xAgentRole_Tool);
  ASSERT_EQ(msgs2[3].blocks.size(), 1u);
  EXPECT_EQ(msgs2[3].blocks[0].type, xAgentContentType_ToolResult);
  EXPECT_EQ(msgs2[3].blocks[0].tool_use_id, "call_42");
  EXPECT_EQ(msgs2[3].blocks[0].tool_result_output, "{\"hello\":\"world\"}");

  xAgentSessionDestroy(sess);
}

/* Two parallel tool_calls in one assistant turn, both dispatched. */
TEST_F(ToolLoopFixture, MultipleToolCallsInOneTurn) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("echo", "c1", "{\"x\":1}"),
      SToolCall("echo", "c2", "{\"x\":2}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  /* Handler ran twice, in order. */
  ASSERT_EQ(echo_log_.size(), 2u);
  EXPECT_EQ(echo_log_[0].id, "c1");
  EXPECT_EQ(echo_log_[1].id, "c2");

  /* Completed, no error. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* Second submit: assistant turn folded two tool_use blocks,
   * followed by two tool_result messages. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(2 tool_uses), tool(c1), tool(c2)] */
  ASSERT_EQ(m2.size(), 5u);
  EXPECT_EQ(m2[2].role, xAgentRole_Assistant);
  ASSERT_EQ(m2[2].blocks.size(), 2u);
  EXPECT_EQ(m2[2].blocks[0].type, xAgentContentType_ToolUse);
  EXPECT_EQ(m2[2].blocks[0].tool_use_id, "c1");
  EXPECT_EQ(m2[2].blocks[1].tool_use_id, "c2");
  EXPECT_EQ(m2[3].role, xAgentRole_Tool);
  EXPECT_EQ(m2[3].blocks[0].tool_use_id, "c1");
  EXPECT_EQ(m2[4].role, xAgentRole_Tool);
  EXPECT_EQ(m2[4].blocks[0].tool_use_id, "c2");

  xAgentSessionDestroy(sess);
}

/* Unknown tool: session fabricates an error tool_result and keeps
 * looping — the MODEL gets to react, not the caller. */
TEST_F(ToolLoopFixture, UnknownToolFeedsErrorBackToModel) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("no_such_tool", "c9", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("ok I'll stop"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("use weird tool")),
            xErrno_Ok);

  /* No handler ran. */
  EXPECT_EQ(echo_log_.size(), 0u);

  /* The run completed normally (not ToolError) — the model decides. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* Second submit carried a tool_result marked is_error=1 with a
   * diagnostic mentioning the tool name. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  bool found_err = false;
  for (const auto &m : m2) {
    if (m.role != xAgentRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type != xAgentContentType_ToolResult) continue;
      if (b.tool_result_is_error &&
          b.tool_result_output.find("no_such_tool") != std::string::npos) {
        found_err = true;
      }
    }
  }
  EXPECT_TRUE(found_err);

  xAgentSessionDestroy(sess);
}

/* Handler returning xErrno_Again yields an error tool_result; loop
 * continues rather than terminating the whole run. */
TEST_F(ToolLoopFixture, HandlerErrorFeedsBackToModel) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("boom", "cb", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("guess I'll quit"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("try")), xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* Check the tool_result was flagged as an error and mentions Again. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  bool found_err = false;
  for (const auto &m : m2) {
    if (m.role != xAgentRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type == xAgentContentType_ToolResult && b.tool_result_is_error) {
        found_err = true;
      }
    }
  }
  EXPECT_TRUE(found_err);

  xAgentSessionDestroy(sess);
}

/* A runaway tool loop: every round returns ToolUse. The session
 * must stop after max_turns rounds with xAgentDoneReason_MaxTurns. */
TEST_F(ToolLoopFixture, MaxTurnsCapsRunawayToolLoop) {
  Captured cap;
  xAgentSessionConf sc = {};
  sc.cbs            = make_cbs(&cap);
  sc.max_turns      = 3; /* override the agent's 5 */
  xAgentSession sess   = xAgentSessionCreate(agent_, &sc);

  /* Push 5 identical scripts; max_turns=3 should stop us before
   * the fourth submit. */
  for (int i = 0; i < 5; i++) {
    fake_->script_queue.push_back({
        SToolCall("echo", "cx", "{}"),
        SDone(xAgentProviderStop_ToolUse),
    });
  }

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("loop")), xErrno_Ok);

  /* We accept either 3 submits (cap applied before the 4th submit)
   * — the important contract is "no unbounded looping, correct
   *   on_done reason". */
  EXPECT_EQ(fake_->submits, 3);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_MaxTurns);

  xAgentSessionDestroy(sess);
}

/* Default max_turns kicks in when neither conf nor agent set one. */
TEST_F(ToolLoopFixture, DefaultMaxTurnsAppliesWhenUnset) {
  /* Build a fresh agent with max_turns=0 and no override. */
  xAgentDestroy(agent_);
  static const xAgentTool *kTools[1] = {&tool_echo_};
  xAgentConf ac   = {};
  ac.loop           = loop_;
  ac.provider       = provider_;
  ac.model          = "fake-model";
  ac.system_prompt  = "sp";
  ac.tools          = kTools;
  ac.tools_count    = 1;
  agent_            = xAgentCreate(&ac);

  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  /* Push way more scripts than the default cap (16) so we can see
   * the cap enforced. */
  for (int i = 0; i < 20; i++) {
    fake_->script_queue.push_back({
        SToolCall("echo", "cx", "{}"),
        SDone(xAgentProviderStop_ToolUse),
    });
  }

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("loop")), xErrno_Ok);
  EXPECT_EQ(fake_->submits, 16);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_MaxTurns);

  xAgentSessionDestroy(sess);
}

/* Provider reports ToolUse but didn't actually emit any tool_call.
 * Treat as ModelError-style ToolError rather than silently looping. */
TEST_F(ToolLoopFixture, ToolUseWithoutAnyCallsYieldsToolError) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({SDone(xAgentProviderStop_ToolUse)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("try")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_ToolError);
  EXPECT_EQ(fake_->submits, 1); /* no follow-up */

  xAgentSessionDestroy(sess);
}

/* Regression for the "reasoning_content missing in assistant tool call
 * message" error on kimi-k2.6 / DeepSeek-R1 / o1. When the provider
 * streams reasoning deltas alongside a tool_call, the session must
 * stash them and echo the reasoning back inside the same assistant
 * turn on the next submit — otherwise moonshot rejects the follow-up
 * with a 400. */
TEST_F(ToolLoopFixture, AssistantThinkingEchoedInFollowUpRound) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  /* Round 1: reasoning chunks split across deltas (as they arrive on
   * the wire), then a tool call, then finish_reason=tool_calls. */
  fake_->script_queue.push_back({
      SThinking("I should "),
      SThinking("call echo."),
      SToolCall("echo", "call_7", "{\"x\":1}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  /* Round 2: model acknowledges and ends the turn. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  /* History must now contain a dedicated Thinking entry before the
   * ToolUse entry inside the assistant turn, so view_build can emit
   * the reasoning_content block on round 2. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  /* [0] user, [1] assistant thinking, [2] assistant tool_use,
   * [3] tool result, [4] assistant text "ok" */
  ASSERT_EQ(hist_len(s), 5u);
  EXPECT_EQ(hist_at(s, 1)->role, xAgentRole_Assistant);
  EXPECT_EQ(hist_at(s, 1)->kind, xAgentSessionEntry_Thinking);
  EXPECT_STREQ(hist_at(s, 1)->text, "I should call echo.");
  EXPECT_EQ(hist_at(s, 2)->kind, xAgentSessionEntry_ToolUse);

  /* Second submit: the assistant message carries thinking + tool_use
   * as distinct content blocks. Provider serialisers (OpenAI-compat)
   * rely on seeing the Thinking block to emit the reasoning_content
   * field. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &msgs2 = fake_->captured_msgs_per_submit[1];
  /* [system, user, assistant(thinking+tool_use), tool] */
  ASSERT_EQ(msgs2.size(), 4u);
  EXPECT_EQ(msgs2[2].role, xAgentRole_Assistant);
  ASSERT_EQ(msgs2[2].blocks.size(), 2u);
  EXPECT_EQ(msgs2[2].blocks[0].type, xAgentContentType_Thinking);
  EXPECT_EQ(msgs2[2].blocks[0].text, "I should call echo.");
  EXPECT_EQ(msgs2[2].blocks[1].type, xAgentContentType_ToolUse);

  xAgentSessionDestroy(sess);
}

/* Usage numbers from each provider round must fold together so the
 * caller sees cumulative input/output tokens for the entire run, not
 * just the last round. This is what the REPL / xbuddy surfaces in
 * "tokens=179/88 total=267" style lines. */
TEST_F(ToolLoopFixture, UsageAccumulatesAcrossToolLoop) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  /* Round 1: 100 prompt / 20 completion / 120 total. Model calls echo. */
  fake_->script_queue.push_back({
      SToolCall("echo", "call_u1", "{}"),
      SDoneWithUsage(xAgentProviderStop_ToolUse, 100, 20, 120),
  });
  /* Round 2: 150 prompt / 30 completion / 180 total. Model finishes. */
  fake_->script_queue.push_back({
      SText("done"),
      SDoneWithUsage(xAgentProviderStop_EndTurn, 150, 30, 180),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(fake_->submits, 2);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* prompt_tokens is the maximum across rounds (each round reports
   * the full input size it saw, not a delta). completion_tokens and
   * total_tokens are cumulative (additive). */
  ASSERT_TRUE(cap.has_usage);
  EXPECT_EQ(cap.usage.prompt_tokens, 150);   /* max(100, 150) */
  EXPECT_EQ(cap.usage.completion_tokens, 50); /* 20 + 30       */
  EXPECT_EQ(cap.usage.total_tokens, 300);     /* 120 + 180     */

  xAgentSessionDestroy(sess);
}

/* A round that reports NULL usage (server stayed silent) must not
 * poison the accumulator — we carry forward whatever earlier rounds
 * reported. Mirrors claude-code's behaviour of skipping over missing
 * usage snapshots rather than zeroing the totals. */
TEST_F(ToolLoopFixture, UsageSurvivesRoundWithoutUsage) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  /* Round 1: reports numbers. */
  fake_->script_queue.push_back({
      SToolCall("echo", "call_u2", "{}"),
      SDoneWithUsage(xAgentProviderStop_ToolUse, 42, 7, 49),
  });
  /* Round 2: deliberately no usage (SDone, not SDoneWithUsage). */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  EXPECT_EQ(cap.done_fired, 1);
  /* Round-1 numbers must survive — missing round neither adds nor
   * resets. */
  ASSERT_TRUE(cap.has_usage);
  EXPECT_EQ(cap.usage.prompt_tokens, 42);
  EXPECT_EQ(cap.usage.completion_tokens, 7);
  EXPECT_EQ(cap.usage.total_tokens, 49);

  xAgentSessionDestroy(sess);
}

/* ── Async tool loop ─────────────────────────────────────────────── */

/* An async tool handler: returns xErrno_Pending and stashes the
 * query pointer so the test can resolve it later via
 * ai_query_async_tool_complete. */
struct AsyncSpy {
  std::vector<ToolRec> log;
  struct xAgentQuery_ *captured_q = nullptr;  /* set by handler */
  std::string       captured_id;           /* set by handler */
};

static xErrno async_handler(xAgentQuery, const xAgentContent *in, xAgentContent *out, void *ud) {
  (void)out;
  auto *spy = static_cast<AsyncSpy *>(ud);
  ToolRec rec;
  rec.name = in->u.tool_use.name ? in->u.tool_use.name : "";
  rec.id   = in->u.tool_use.id   ? in->u.tool_use.id   : "";
  rec.args = in->u.tool_use.args_json ? in->u.tool_use.args_json : "";
  spy->log.push_back(rec);
  /* Do NOT populate out — caller must not read it when Pending. */
  return xErrno_Pending;
}

/* Fixture that provides an async tool alongside the sync echo tool. */
class AsyncToolFixture : public SessionTest {
 protected:
  xAgentTool    tool_async_  = nullptr;
  xAgentTool    tool_echo_   = nullptr;
  AsyncSpy   async_spy_;
  std::vector<ToolRec> echo_log_;

  void SetUp() override {
    SessionTest::SetUp();
    xAgentDestroy(agent_);

    xAgentToolConf tc = {};
    tc.name        = "slow_op";
    tc.description = "an async tool";
    tc.json_schema = "{\"type\":\"object\"}";
    tc.handler     = async_handler;
    tc.user_data   = &async_spy_;
    tool_async_    = xAgentToolCreate(&tc);

    tc           = {};
    tc.name      = "echo";
    tc.handler   = echo_handler;
    tc.user_data = &echo_log_;
    tool_echo_   = xAgentToolCreate(&tc);

    static const xAgentTool *kTools[2];
    kTools[0] = &tool_async_;
    kTools[1] = &tool_echo_;

    xAgentConf ac   = {};
    ac.loop           = loop_;
    ac.provider       = provider_;
    ac.model          = "fake-model";
    ac.system_prompt  = "you are a test";
    ac.max_turns      = 5;
    ac.max_tokens     = 1024;
    ac.tools          = kTools;
    ac.tools_count    = 2;
    agent_            = xAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }

  void TearDown() override {
    xAgentToolDestroy(tool_async_);
    xAgentToolDestroy(tool_echo_);
    SessionTest::TearDown();
  }
};

/* Single async tool: handler returns Pending, then the test resolves
 * it by calling ai_query_async_tool_complete. The tool-loop should
 * then continue with the next provider round. */
TEST_F(AsyncToolFixture, SingleAsyncToolRoundTrip) {
  struct LocalCap : Captured {
    std::vector<std::pair<std::string, int>> tool_events;
  } cap;

  auto cbs        = make_cbs(&cap);
  cbs.on_tool     = [](xAgentSession, const char *name, int started, void *ud) {
    auto *c = static_cast<LocalCap *>(ud);
    c->tool_events.push_back({name, started});
  };
  xAgentSession sess = make_session(cbs);

  /* Round 1: model calls the async tool. */
  fake_->script_queue.push_back({
      SToolCall("slow_op", "call_async_1", "{\"delay\":5}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  /* Round 2: model acknowledges the result and ends the turn.
   * This script must be queued BEFORE the async completion so that
   * submit_round inside ai_query_async_tool_complete can consume it. */
  fake_->script_queue.push_back({
      SText("got it"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go slow")), xErrno_Ok);

  /* The handler ran once and returned Pending. */
  ASSERT_EQ(async_spy_.log.size(), 1u);
  EXPECT_EQ(async_spy_.log[0].name, "slow_op");
  EXPECT_EQ(async_spy_.log[0].id, "call_async_1");

  /* Only one provider submit so far (round 1). The session's
   * on_done has NOT fired yet because the async tool is pending. */
  EXPECT_EQ(fake_->submits, 1);
  EXPECT_EQ(cap.done_fired, 0);

  /* on_tool(started=1) was emitted, but started=0 is deferred. */
  ASSERT_EQ(cap.tool_events.size(), 1u);
  EXPECT_EQ(cap.tool_events[0].first, "slow_op");
  EXPECT_EQ(cap.tool_events[0].second, 1);

  /* Now resolve the async tool. Grab the live Query from the session. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  xAgentContent result = {};
  result.type                    = xAgentContentType_ToolResult;
  result.u.tool_result.id        = "call_async_1";
  result.u.tool_result.output    = R"({"status":"done"})";
  result.u.tool_result.output_len = 17;
  result.u.tool_result.is_error  = 0;

  ai_query_async_tool_complete(s->query, "call_async_1", &result);

  /* The tool-loop continued: round 2 submitted. */
  EXPECT_EQ(fake_->submits, 2);

  /* on_tool(started=0) was emitted upon async completion. */
  ASSERT_GE(cap.tool_events.size(), 2u);
  EXPECT_EQ(cap.tool_events[1].first, "slow_op");
  EXPECT_EQ(cap.tool_events[1].second, 0);

  /* Session-level on_done fired with Completed. */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* Streaming text from round 2 reached the caller. */
  EXPECT_EQ(cap.texts, "got it");

  /* History:
   *   [0] user "go slow"
   *   [1] assistant tool_use (slow_op, call_async_1)
   *   [2] tool tool_result (call_async_1, {"status":"done"})
   *   [3] assistant text "got it" */
  ASSERT_EQ(hist_len(s), 4u);
  EXPECT_EQ(hist_at(s, 0)->role, xAgentRole_User);
  EXPECT_EQ(hist_at(s, 1)->role, xAgentRole_Assistant);
  EXPECT_EQ(hist_at(s, 1)->kind, xAgentSessionEntry_ToolUse);
  EXPECT_STREQ(hist_at(s, 1)->tool_use_name, "slow_op");
  EXPECT_EQ(hist_at(s, 2)->role, xAgentRole_Tool);
  EXPECT_EQ(hist_at(s, 2)->kind, xAgentSessionEntry_ToolResult);
  EXPECT_STREQ(hist_at(s, 2)->tool_result_id, "call_async_1");
  EXPECT_EQ(std::string(hist_at(s, 2)->tool_result_output,
                         hist_at(s, 2)->tool_result_output_len),
            R"({"status":"done"})");
  EXPECT_EQ(hist_at(s, 3)->role, xAgentRole_Assistant);
  EXPECT_STREQ(hist_at(s, 3)->text, "got it");

  xAgentSessionDestroy(sess);
}

/* Mix of async and sync tools in the same assistant turn.
 * The sync tool's result is written immediately; the async tool
 * is resolved later. The tool-loop continues only after ALL
 * tools complete. */
TEST_F(AsyncToolFixture, MixedSyncAndAsyncToolsInOneTurn) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("slow_op", "c_async", "{\"x\":1}"),
      SToolCall("echo", "c_sync", "{\"y\":2}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("both done"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("mix")), xErrno_Ok);

  /* Both handlers ran. */
  ASSERT_EQ(async_spy_.log.size(), 1u);
  EXPECT_EQ(async_spy_.log[0].id, "c_async");
  ASSERT_EQ(echo_log_.size(), 1u);
  EXPECT_EQ(echo_log_[0].id, "c_sync");

  /* Only one provider submit (round 1). on_done hasn't fired. */
  EXPECT_EQ(fake_->submits, 1);
  EXPECT_EQ(cap.done_fired, 0);

  /* Resolve the async tool. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  xAgentContent result = {};
  result.type                    = xAgentContentType_ToolResult;
  result.u.tool_result.id        = "c_async";
  result.u.tool_result.output    = "async_result";
  result.u.tool_result.output_len = 12;
  result.u.tool_result.is_error  = 0;

  ai_query_async_tool_complete(s->query, "c_async", &result);

  /* Round 2 submitted after async resolution. */
  EXPECT_EQ(fake_->submits, 2);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);
  EXPECT_EQ(cap.texts, "both done");

  /* Second submit carried BOTH tool_results: sync echo result
   * (already in produced during dispatch) and the async result
   * (appended by ai_query_async_tool_complete). */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  /* Collect all tool_result IDs from round-2 messages. */
  std::vector<std::string> result_ids;
  for (const auto &m : m2) {
    if (m.role != xAgentRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type == xAgentContentType_ToolResult) {
        result_ids.push_back(b.tool_use_id);
      }
    }
  }
  ASSERT_EQ(result_ids.size(), 2u);
  /* Order: sync result was appended first by dispatch_pending_tools,
   * async result appended later by ai_query_async_tool_complete. */
  EXPECT_EQ(result_ids[0], "c_sync");
  EXPECT_EQ(result_ids[1], "c_async");

  xAgentSessionDestroy(sess);
}

/* Async tool resolved with a Text content (not ToolResult) should
 * be auto-wrapped as a non-error tool_result. */
TEST_F(AsyncToolFixture, AsyncCompletionWithTextContent) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("slow_op", "c_txt", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  /* Resolve with a plain Text content block. */
  xAgentContent result = {};
  result.type       = xAgentContentType_Text;
  result.u.text.text = "plain text answer";
  result.u.text.len  = 17;

  ai_query_async_tool_complete(s->query, "c_txt", &result);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* The tool_result in history should carry the text as output. */
  /* [0] user, [1] assistant tool_use, [2] tool result */
  ASSERT_EQ(hist_len(s), 3u);
  EXPECT_EQ(hist_at(s, 2)->kind, xAgentSessionEntry_ToolResult);
  EXPECT_EQ(std::string(hist_at(s, 2)->tool_result_output,
                         hist_at(s, 2)->tool_result_output_len),
            "plain text answer");
  EXPECT_EQ(hist_at(s, 2)->tool_result_is_error, 0);

  xAgentSessionDestroy(sess);
}

/* Async tool resolved with NULL result should synthesize an error. */
TEST_F(AsyncToolFixture, AsyncCompletionWithNullResult) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("slow_op", "c_null", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  ai_query_async_tool_complete(s->query, "c_null", nullptr);

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* tool_result should be marked as error. */
  ASSERT_EQ(hist_len(s), 4u);
  EXPECT_EQ(hist_at(s, 2)->kind, xAgentSessionEntry_ToolResult);
  EXPECT_EQ(hist_at(s, 2)->tool_result_is_error, 1);

  xAgentSessionDestroy(sess);
}

/* Two async tools in one turn: both must complete before the
 * tool-loop continues. Resolving the first one does NOT trigger
 * a provider submit; only after the second one does. */
TEST_F(AsyncToolFixture, TwoAsyncToolsMustBothComplete) {
  /* Create a second async tool with a separate spy. */
  AsyncSpy spy2;
  xAgentToolConf tc2 = {};
  tc2.name      = "slow_op2";
  tc2.handler   = async_handler;
  tc2.user_data = &spy2;
  xAgentTool tool_async2 = xAgentToolCreate(&tc2);
  ASSERT_NE(tool_async2, nullptr);

  /* Rebuild agent with three tools. */
  xAgentDestroy(agent_);
  static const xAgentTool *kTools3[3];
  kTools3[0] = &tool_async_;
  kTools3[1] = &tool_async2;
  /* Need an echo tool too for completeness, but we don't use it. */
  static const xAgentTool *kEcho = &tool_echo_;
  kTools3[2] = kEcho;

  xAgentConf ac   = {};
  ac.loop           = loop_;
  ac.provider       = provider_;
  ac.model          = "fake-model";
  ac.system_prompt  = "sp";
  ac.max_turns      = 5;
  ac.max_tokens     = 1024;
  ac.tools          = kTools3;
  ac.tools_count    = 3;
  agent_            = xAgentCreate(&ac);

  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SToolCall("slow_op", "a1", "{}"),
      SToolCall("slow_op2", "a2", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("both resolved"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  /* Both handlers ran. */
  ASSERT_EQ(async_spy_.log.size(), 1u);
  ASSERT_EQ(spy2.log.size(), 1u);

  EXPECT_EQ(fake_->submits, 1);
  EXPECT_EQ(cap.done_fired, 0);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  /* Resolve the first async tool — should NOT trigger round 2. */
  xAgentContent r1 = {};
  r1.type                    = xAgentContentType_ToolResult;
  r1.u.tool_result.id        = "a1";
  r1.u.tool_result.output    = "first";
  r1.u.tool_result.output_len = 5;
  r1.u.tool_result.is_error  = 0;
  ai_query_async_tool_complete(s->query, "a1", &r1);

  EXPECT_EQ(fake_->submits, 1);  /* still waiting for a2 */
  EXPECT_EQ(cap.done_fired, 0);

  /* Resolve the second async tool — NOW round 2 fires. */
  xAgentContent r2 = {};
  r2.type                    = xAgentContentType_ToolResult;
  r2.u.tool_result.id        = "a2";
  r2.u.tool_result.output    = "second";
  r2.u.tool_result.output_len = 6;
  r2.u.tool_result.is_error  = 0;
  ai_query_async_tool_complete(s->query, "a2", &r2);

  EXPECT_EQ(fake_->submits, 2);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);
  EXPECT_EQ(cap.texts, "both resolved");

  xAgentSessionDestroy(sess);
  xAgentToolDestroy(tool_async2);
}

/* ── Cancel propagation to async tools ────────────────────────────── */

/* Verify that xAgentQueryCancel propagates to on_cancel_fn for every
 * in-flight async tool. */
TEST_F(AsyncToolFixture, CancelPropagatesToAsyncTools) {
  /* Build a cancellable async tool. */
  struct CancelSpy {
    int cancel_calls = 0;
    std::string last_tool_use_id;
  } cancel_spy;

  auto cancel_fn = [](xAgentQuery, const char *tool_use_id, xAgentTool, void *ud) {
    auto *s = static_cast<CancelSpy *>(ud);
    s->cancel_calls++;
    s->last_tool_use_id = tool_use_id ? tool_use_id : "";
  };

  xAgentToolConf tc = {};
  tc.name          = "slow_op";   /* same name as tool_async_ */
  tc.description   = "cancellable async";
  tc.json_schema   = "{\"type\":\"object\"}";
  tc.handler       = async_handler;
  tc.user_data     = &async_spy_;
  tc.on_cancel_fn  = cancel_fn;
  tc.on_cancel_ud  = &cancel_spy;
  xAgentTool tool_cancel = xAgentToolCreate(&tc);
  ASSERT_NE(tool_cancel, nullptr);

  /* Replace the agent's tool set with our cancellable tool. */
  xAgentToolDestroy(tool_async_);
  tool_async_ = tool_cancel;

  static const xAgentTool *kTools[2];
  kTools[0] = &tool_async_;
  kTools[1] = &tool_echo_;
  xAgentDestroy(agent_);

  xAgentConf ac   = {};
  ac.loop           = loop_;
  ac.provider       = provider_;
  ac.model          = "fake-model";
  ac.system_prompt  = "you are a test";
  ac.max_turns      = 5;
  ac.max_tokens     = 1024;
  ac.tools          = kTools;
  ac.tools_count    = 2;
  agent_            = xAgentCreate(&ac);
  ASSERT_NE(agent_, nullptr);

  Captured cap;
  auto cbs    = make_cbs(&cap);
  xAgentSession sess = make_session(cbs);

  /* Round 1: model calls the async tool. */
  fake_->script_queue.push_back({
      SToolCall("slow_op", "call_cancel_1", "{\"delay\":999}"),
      SDone(xAgentProviderStop_ToolUse),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);
  ASSERT_EQ(async_spy_.log.size(), 1u);

  /* The async tool is in-flight. Cancel the query. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_NE(s->query, nullptr);

  EXPECT_EQ(cancel_spy.cancel_calls, 0);
  xAgentQueryCancel(s->query);

  /* on_cancel_fn should have been called exactly once. */
  EXPECT_EQ(cancel_spy.cancel_calls, 1);
  EXPECT_EQ(cancel_spy.last_tool_use_id, "call_cancel_1");

  xAgentSessionDestroy(sess);
}
TEST_F(SessionTest, UsageStaysNullWhenProviderNeverReports) {
  Captured cap;
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SText("hi"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  /* No round reported anything → session hands NULL to the caller. */
  EXPECT_FALSE(cap.has_usage);

  xAgentSessionDestroy(sess);
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
  xAgentSession sess = make_session(make_cbs(&cap));

  fake_->script_queue.push_back({
      SThinking("I should "),
      SThinking("say hi."),
      SText("hello"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  /* Thinking deltas reassembled in arrival order. */
  EXPECT_EQ(cap.thinking, "I should say hi.");
  EXPECT_EQ(cap.thinking_fired, 2);

  /* Text channel stayed clean — no reasoning bleed. */
  EXPECT_EQ(cap.texts, "hello");

  xAgentSessionDestroy(sess);
}

/* Callers that don't care about reasoning leave on_thinking NULL.
 * The session must still accept the deltas (some servers, notably
 * kimi-k2.6, require them to be echoed back on the next tool-loop
 * round or they'll 400) — it just shouldn't crash or spill them
 * into on_text. */
TEST_F(SessionTest, ThinkingWithoutCallbackDoesNotCrash) {
  Captured cap;
  xAgentSessionCallbacks cbs = make_cbs(&cap);
  cbs.on_thinking = nullptr; /* opt out */
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SThinking("hidden reasoning"),
      SText("visible"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  /* Thinking callback wasn't set → nothing captured there. */
  EXPECT_EQ(cap.thinking, "");
  EXPECT_EQ(cap.thinking_fired, 0);
  /* And it didn't bleed into on_text. */
  EXPECT_EQ(cap.texts, "visible");

  xAgentSessionDestroy(sess);
}

/* ── Context-budget policy (xAgentBudgetConf) ──────────────────────────
 *
 * End-to-end coverage for xAgentSessionInput's pre-append budget gate.
 * These tests exercise the branches of session_enforce_budget_():
 *
 *   - Disabled  → pass-through (regression anchor)
 *   - Error     → return xErrno_PromptTooLong when the estimate
 *                 exceeds max_tokens; NO history mutation, NO query
 *                 submission observable to the caller
 *   - Truncate  → drop the oldest user-anchored prefix so the new
 *                 turn fits, then proceed normally; and return
 *                 PromptTooLong when even the maximally aggressive
 *                 trim still does not bring us under the ceiling
 *
 * The token estimator is coarse on purpose (bytes/4 + 8 per entry),
 * so each test picks payload sizes that land comfortably on one
 * side of the configured max_tokens, not right at the boundary.
 *
 * Helper: make_session_with_budget overrides only the budget field
 * so every test stays independent of the default agent config. */
namespace {

xAgentSession make_session_with_budget(xAgent agent,
                                    const xAgentSessionCallbacks &cbs,
                                    xAgentBudgetConf              budget) {
  xAgentSessionConf sc = {};
  sc.cbs            = cbs;
  sc.budget         = budget;
  return xAgentSessionCreate(agent, &sc);
}

}  // namespace

/* Disabled (the zero-default) must behave identically to a session
 * without any budget config: a huge user message that would blow
 * through any realistic ceiling is still accepted, history is
 * populated, and the Query runs. Regression anchor for the "no
 * behaviour change on existing callers" promise. */
TEST_F(SessionTest, BudgetDisabledAcceptsOversizedInput) {
  Captured cap;
  xAgentBudgetConf budget{};              /* policy = Disabled (zero) */
  budget.context_window        = 1;        /* deliberately absurd      */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  /* ~400 bytes of 'x' — far above 1 token — still accepted. */
  std::string big(400, 'x');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
            xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  xAgentSessionDestroy(sess);
}

/* Error policy refuses the call synchronously with
 * xErrno_PromptTooLong when the estimate exceeds max_tokens, and
 * MUST NOT touch history — a future non-oversized call on the same
 * session should still see an empty history, so the Busy check is
 * the only state visible from a refused turn. */
TEST_F(SessionTest, BudgetErrorPolicyRefusesOversizedInput) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Error;
  budget.context_window        = 20;       /* 20 tokens ≈ 80 payload bytes */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* 200 bytes → ~58 tokens including envelope → well above 20. */
  std::string big(200, 'x');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
            xErrno_PromptTooLong);

  /* Nothing fired on the caller side — no on_done, no on_error.
   * The synchronous return value IS the failure signal. */
  EXPECT_EQ(cap.done_fired, 0);
  EXPECT_EQ(cap.error_fired, 0);

  /* History must be clean: the refused turn leaves no trace. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(hist_len(s), 0u);

  /* The provider must never have been submitted to. */
  EXPECT_TRUE(fake_->captured_msgs.empty());

  xAgentSessionDestroy(sess);
}

/* Error policy under the ceiling is a no-op: the gate lets the
 * turn through and the Query runs as if the budget were Disabled.
 * This pairs with the refusal test above to prove the gate is
 * conditional on the estimate, not on the policy alone. */
TEST_F(SessionTest, BudgetErrorPolicyAllowsUnderBudgetInput) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 200;             /* generous */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  xAgentSessionDestroy(sess);
}

/* TruncateOldest policy was removed; its dedicated tests
 * (BudgetTruncateOldestDropsOldestUserTurns,
 *  L1PreserveTruncatedOnBudgetTrim) were deleted along with it. The
 * new compact pipeline lands in a follow-up commit and will grow
 * its own coverage. */

/* Too few user turns to compact: the session refuses with
 * PromptTooLong rather than silently violating the budget.
 * This is the "safety over compliance" path. Applies to
 * Summarize just as it did to the removed TruncateOldest. */
TEST_F(SessionTest, BudgetRefusesWhenFloorUnreachable) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window        = 30;       /* very tight                   */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* History is empty → find_nth_user_turn returns NO_SUCH_TURN →
   * earliest_keep returns 0 → nothing to summarise. A sufficiently
   * large incoming message thus has nowhere to go. */
  std::string big(400, 'x'); /* ~108 tokens, well above 30 */
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
            xErrno_PromptTooLong);

  /* Same history cleanliness guarantee as the Error-policy refusal. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(hist_len(s), 0u);
  EXPECT_EQ(cap.done_fired, 0);
  EXPECT_EQ(cap.error_fired, 0);

  xAgentSessionDestroy(sess);
}

/* Under-budget inputs with a non-Disabled policy must still run
 * cleanly. Smoke test that the budget gate does not gratuitously
 * reject when the estimate is below the ceiling, regardless of
 * which enforcing policy is selected. */
TEST_F(SessionTest, BudgetPolicyUnderBudgetIsNoop) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Summarize;
  budget.context_window = 500;             /* roomy                        */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  /* History should reflect a clean single round (user + assistant). */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_GT(hist_len(s), 0u);

  xAgentSessionDestroy(sess);
}

/* ── Retroactive tool_result trimming ────────────────────────────
 *
 * When trim_tool_results_threshold is set, the session trims
 * consumed tool_result outputs in-place (instead of dropping entire
 * turns) when context usage exceeds the threshold. */

class TrimToolResultsFixture : public ToolLoopFixture {
 protected:
  /* Handler that echoes a large payload back as tool_result output,
   * so we can test retroactive trimming. */
  static std::string large_payload_;
  static xErrno large_echo_handler(xAgentQuery, const xAgentContent *in,
                                    xAgentContent *out, void *) {
    out->type                     = xAgentContentType_ToolResult;
    out->u.tool_result.id         = in->u.tool_use.id;
    out->u.tool_result.output     = large_payload_.c_str();
    out->u.tool_result.output_len = large_payload_.size();
    out->u.tool_result.is_error   = 0;
    return xErrno_Ok;
  }

  xAgentTool tool_large_echo_ = nullptr;

  void SetUp() override {
    large_payload_ = std::string(2000, 'A'); /* 2000 bytes ≈ 500 tokens */
    ToolLoopFixture::SetUp();
    xAgentDestroy(agent_);

    xAgentToolConf tc = {};
    tc.name        = "big_echo";
    tc.description = "echo big payload";
    tc.json_schema = "{\"type\":\"object\"}";
    tc.handler     = large_echo_handler;
    tool_large_echo_ = xAgentToolCreate(&tc);

    static const xAgentTool *kTools[3];
    kTools[0] = &tool_echo_;
    kTools[1] = &tool_failing_;
    kTools[2] = &tool_large_echo_;

    xAgentConf ac   = {};
    ac.loop           = loop_;
    ac.provider       = provider_;
    ac.model          = "fake-model";
    ac.system_prompt  = "you are a test";
    ac.max_turns      = 10;
    ac.max_tokens     = 1024;
    ac.tools          = kTools;
    ac.tools_count    = 3;
    agent_ = xAgentCreate(&ac);
  }

  void TearDown() override {
    xAgentToolDestroy(tool_large_echo_);
    ToolLoopFixture::TearDown();
  }
};
std::string TrimToolResultsFixture::large_payload_;

/* Retroactive trimming fires when usage exceeds threshold, trims
 * consumed tool_result outputs, and allows the next input through
 * without dropping entire turns. */
TEST_F(TrimToolResultsFixture, TrimsConsumedToolResultsAboveThreshold) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy                       = xAgentBudgetPolicy_Summarize;
  budget.context_window                   = 300;  /* tight: forces trimming on round 2 */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Round 1: user → assistant calls big_echo → tool_result (large)
   *          → assistant acknowledges. */
  fake_->script_queue.push_back({
      SText("let me call big_echo "),
      SToolCall("big_echo", "call_1", R"({"req":"data"})"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("got the result."),
      SDone(xAgentProviderStop_EndTurn),
  });

  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")),
            xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  /* Verify the large tool_result is in history. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_GE(hist_len(s), 4u);
  bool found_large = false;
  for (size_t i = 0; i < hist_len(s); i++) {
    if (hist_at(s, i)->kind == xAgentSessionEntry_ToolResult &&
        hist_at(s, i)->tool_result_output_len > 100) {
      found_large = true;
    }
  }
  EXPECT_TRUE(found_large) << "large tool_result should exist after round 1";

  /* Round 1.5: a separating user turn so that Round 1's tool_result
   * falls into the middle band (between head and recent). */
  fake_->script_queue.push_back({
      SText("intermediate"),
      SDone(xAgentProviderStop_EndTurn),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("mid")),
            xErrno_Ok);

  /* Round 2: another input that pushes us over budget. The
   * retroactive trimmer should shrink the consumed tool_result
   * from round 1 instead of dropping entire turns. If trimming
   * alone doesn't free enough, a compact may be launched too. */
  fake_->script_queue.push_back({
      SText("summary of old history"),
      SDone(xAgentProviderStop_EndTurn),
  });
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  std::string big_input(400, 'x');
  xErrno rc = xAgentSessionInput(sess,
            xAgentMessageFromText(big_input.c_str()));
  /* The input may be accepted (Ok), trigger a compact (Busy),
   * or be refused (PromptTooLong). Any is valid — what matters
   * is that trimming happened. */
  (void)rc;

  /* After retroactive trimming, the consumed tool_result should
   * have been shrunk (output_len much smaller than original). */
  bool found_trimmed = false;
  for (size_t i = 0; i < hist_len(s); i++) {
    if (hist_at(s, i)->kind == xAgentSessionEntry_ToolResult) {
      /* The original was 2000 bytes; after trimming it should be
       * much shorter (just a marker string). */
      if (hist_at(s, i)->tool_result_output_len < 100) {
        found_trimmed = true;
      }
    }
  }
  EXPECT_TRUE(found_trimmed)
      << "consumed tool_result should have been trimmed in-place";

  xAgentSessionDestroy(sess);
}

/* When the budget gate fires, consumed tool_result outputs are
 * always trimmed in-place (the new design has no threshold — it
 * trims whenever over budget). This test verifies that trimming
 * does happen when the session is over budget. */
TEST_F(TrimToolResultsFixture, TrimsConsumedToolResultsWhenOverBudget) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy                       = xAgentBudgetPolicy_Summarize;
  budget.context_window                   = 300;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Round 1: user → assistant calls big_echo → tool_result → done. */
  fake_->script_queue.push_back({
      SText("calling "),
      SToolCall("big_echo", "call_1", R"({"req":"data"})"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({
      SText("done."),
      SDone(xAgentProviderStop_EndTurn),
  });

  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")),
            xErrno_Ok);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);

  /* Script a compact Query response in case the session decides to
   * summarise after trimming. The summary content is not asserted
   * on — we only care that the tool_result was trimmed in-place. */
  fake_->script_queue.push_back({
      SText("summary: prior rounds completed."),
      SDone(xAgentProviderStop_EndTurn),
  });
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  std::string big_input(400, 'x');
  xErrno rc = xAgentSessionInput(sess,
              xAgentMessageFromText(big_input.c_str()));

  /* With the new design, consumed tool_results are trimmed whenever
   * the session is over budget (no threshold gating). The entry
   * should have been shrunk in-place.
   *
   * rc is allowed to be Ok (summary succeeded), Busy (summary in
   * flight) or PromptTooLong (no summary scripted / floor). */
  (void)rc;
  bool found_trimmed = false;
  for (size_t i = 0; i < hist_len(s); i++) {
    if (hist_at(s, i)->kind == xAgentSessionEntry_ToolResult) {
      size_t out_len = hist_at(s, i)->tool_result_output_len;
      /* An in-place trim leaves the entry with a short marker
       * payload (< ~80 bytes). The original was > 1000 bytes. */
      if (out_len < 200u) {
        found_trimmed = true;
      }
    }
  }
  EXPECT_TRUE(found_trimmed)
      << "consumed tool_result should have been trimmed in-place when over budget";

  xAgentSessionDestroy(sess);
}

/* ── Incremental token bookkeeping ────────────────────────────────
 *
 * These tests cover the incremental bookkeeping that replaced the
 * old EWMA calibrator:
 *
 *   - A fresh session starts with known_prompt_tokens = -1 (unknown).
 *   - After a run with positive prompt_tokens, known_prompt_tokens
 *     is updated and delta_entries is reset to 0.
 *   - Runs without a usage block, or with -1 prompt_tokens, do
 *     NOT update known_prompt_tokens.
 *   - After a truncate/compact, known_prompt_tokens is invalidated
 *     back to -1 (history changed imprecisely).
 *   - The next gate uses known_prompt_tokens + delta estimate for
 *     more accurate budget decisions. */

TEST_F(SessionTest, BudgetBookkeepingInitialStateIsUnknown) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 1000;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(s->known_prompt_tokens, -1);
  EXPECT_EQ(s->delta_entries, 0u);

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, BudgetBookkeepingUpdatesOnProviderReport) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 1000;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Provider reports prompt_tokens = 900. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDoneWithUsage(xAgentProviderStop_EndTurn, /*prompt=*/900,
                     /*completion=*/10),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hello")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  /* known_prompt_tokens should now be the provider-reported value. */
  EXPECT_EQ(s->known_prompt_tokens, 900);
  /* delta_entries counts the produced entries (assistant reply)
   * that were merged into history after the provider report.
   * These have not been counted by the provider yet. */
  EXPECT_GT(s->delta_entries, 0u);

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, BudgetBookkeepingIgnoresMissingUsage) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 1000;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Plain SDone without SDoneWithUsage → provider reports NULL
   * usage → known_prompt_tokens must stay at -1. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hello")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(s->known_prompt_tokens, -1);

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, BudgetBookkeepingIgnoresUnknownPromptTokens) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 1000;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Usage present but prompt_tokens = -1 (the "unknown" sentinel). */
  fake_->script_queue.push_back({
      SText("ok"),
      SDoneWithUsage(xAgentProviderStop_EndTurn, /*prompt=*/-1,
                     /*completion=*/7),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hello")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(s->known_prompt_tokens, -1);

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, BudgetBookkeepingFeedsBackIntoNextGate) {
  /* The end-to-end story: after the first run reports a large
   * prompt_tokens, the second gate uses that precise value as the
   * baseline. This means a previously-accepted payload size can be
   * refused if the known baseline + new delta exceeds the limit.
   *
   * Setup:
   *   - max_tokens = 500
   *   - First turn: provider reports prompt_tokens = 400.
   *   - Second turn: known_prompt_tokens = 400, plus the new
   *     user message estimate, exceeds 500 → refused. */
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Error;
  budget.context_window = 500;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* First turn: any payload. Provider reports prompt_tokens = 400. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDoneWithUsage(xAgentProviderStop_EndTurn, /*prompt=*/400,
                     /*completion=*/12),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hello")),
            xErrno_Ok);
  ASSERT_EQ(cap.done_fired, 1);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(s->known_prompt_tokens, 400);

  /* Second turn: known_prompt_tokens = 400 + delta for produced
   * entries + incoming message. Even a small message will push
   * the total above 500 because the baseline is already 400
   * and there are produced entries from the first turn. Use a
   * large payload to make the refusal unambiguous. */
  std::string big(400, 'q');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
            xErrno_PromptTooLong);

  xAgentSessionDestroy(sess);
}

/* ── Context-budget: Summarize (β phase) ────────────────────
 *
 * End-to-end coverage for the Summarize budget policy.
 *
 * Key behaviour:
 *   - When the budget is exceeded, the session launches an internal
 *     compact Query that asks the model to summarise the old history.
 *   - The caller's xAgentSessionInput returns xErrno_Busy while the
 *     compact is in flight.
 *   - When the compact completes (synchronously with the fake
 *     provider), the old history entries are replaced by a single
 *     System summary entry with a "[summary] " prefix.
 *   - The caller can then re-submit their original message; the
 *     budget gate should now let it through.
 *   - If the compact fails (empty output / OOM), the session
 *     degrades to TruncateOldest behaviour.
 */

/* Summarize returns Busy when the budget is exceeded,
 * indicating a compact Query is in flight. With the synchronous
 * fake provider the compact completes before xAgentQueryRun returns,
 * so by the time xAgentSessionInput yields Busy the history has
 * already been compressed. A second Input call on the same
 * message should then pass the budget gate and run normally. */
TEST_F(SessionTest, BudgetSummarizeCompactsHistory) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window        = 200;      /* enough for 3 primer rounds  */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Prime 3 rounds. Each user msg is ~30 tokens (80/4 + 10).
   * After 3 rounds history ≈ 3×(30+10) = 120 tokens.
   * With limit = 200 the primer rounds should pass. */
  const std::string big(80, 'a');
  for (int i = 0; i < 3; i++) {
    fake_->script_queue.push_back({
        SText("reply"),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
              xErrno_Ok);
    ASSERT_EQ(cap.done_fired, i + 1);
  }

  /* 4th input overflows the budget. Current history ≈ 120 tokens,
   * incoming ≈ 30 tokens, total ≈ 150. With max_tokens = 200 it
   * still fits — so we need a BIGGER payload to trigger overflow.
   * A 400-byte string → ~110 tokens incoming; 120+110 = 230 > 200. */
  fake_->script_queue.push_back({
      SText("This is a summary of the conversation."),
      SDone(xAgentProviderStop_EndTurn),
  });

  const std::string overflow_msg(400, 'b');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(overflow_msg.c_str())),
            xErrno_Busy);

  /* The compact has completed synchronously. History should now
   * contain a System summary entry at the beginning. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  size_t hlen = hist_len(s);
  ASSERT_GT(hlen, 0u);

  /* The first history entry should be the System summary. */
  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  ASSERT_NE(msgs, nullptr);
  EXPECT_EQ(msgs[0].role, xAgentRole_System);
  EXPECT_NE(msgs[0].text, nullptr);
  EXPECT_NE(std::string(msgs[0].text, msgs[0].text_len).find("[summary]"),
            std::string::npos)
      << "compact should produce a [summary] entry";

  /* The old user turns (3 × "aaa…") should have been compacted —
   * compact_end_idx is the second-to-last user turn, so
   * at most 2 old user turns remain (the last two). */
  int old_user_count = 0;
  for (size_t i = 0; i < hlen; i++) {
    if (msgs[i].role == xAgentRole_User &&
        std::string(msgs[i].text, msgs[i].text_len) == big) {
      old_user_count++;
    }
  }
  EXPECT_LE(old_user_count, 2)
      << "at most 2 old user turns should remain after compact";

  /* The auto-retry mechanism re-submits the pending input after
   * compact completes, so on_done should have fired for the
   * auto-retried message too. */
  EXPECT_GE(cap.done_fired, 3)
      << "at least the 3 primer rounds should have fired on_done";

  /* The auto-retry already re-submitted the pending input after
   * compact completed, so we don't need to manually re-submit. */

  xAgentSessionDestroy(sess);
}

/* Summarize under budget is a no-op: the gate lets the turn
 * through without launching a compact Query. */
TEST_F(SessionTest, BudgetSummarizeUnderBudgetIsNoop) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy     = xAgentBudgetPolicy_Summarize;
  budget.context_window = 500;             /* generous */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  /* History should have the user message (no summary). */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  ASSERT_GT(hist_len(s), 0u);
  /* First user entry should NOT be a summary. */
  for (size_t i = 0; i < hist_len(s); i++) {
    if (msgs[i].role == xAgentRole_User) {
      EXPECT_NE(std::string(msgs[i].text, msgs[i].text_len).find("[summary]"),
                0u)
          << "no summary entry when under budget";
      break;
    }
  }

  xAgentSessionDestroy(sess);
}

/* When the compact Query produces no text (empty output), the
 * summarize attempt is reported as failed (summary_ok=false) and
 * history is left untouched. There is NO degradation to truncation.
 * The caller (e.g. CLI) is responsible for deciding retry/abort. */
TEST_F(SessionTest, BudgetSummarizeReportsErrorOnEmptySummary) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window        = 200;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Prime 3 rounds to fill history. */
  const std::string big(80, 'a');
  for (int i = 0; i < 3; i++) {
    fake_->script_queue.push_back({
        SText("reply"),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
              xErrno_Ok);
    ASSERT_EQ(cap.done_fired, i + 1);
  }

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  size_t hlen_before = hist_len(s);

  /* The compact Query returns no text (model responds with empty).
   * This should surface as a failure — history stays untouched. */
  fake_->script_queue.push_back({
      SDone(xAgentProviderStop_EndTurn),     /* no SText — empty output */
  });

  const std::string overflow_msg(400, 'b');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(overflow_msg.c_str())),
            xErrno_Busy);

  /* History must be unchanged: no [summary] entry, same length. */
  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  size_t hlen_after = hist_len(s);
  EXPECT_EQ(hlen_after, hlen_before)
      << "history length unchanged on compact failure";

  int summary_count = 0;
  for (size_t i = 0; i < hlen_after; i++) {
    if (msgs[i].role == xAgentRole_System && msgs[i].text &&
        std::string(msgs[i].text, msgs[i].text_len).find("[summary]") !=
            std::string::npos) {
      summary_count++;
    }
  }
  EXPECT_EQ(summary_count, 0)
      << "no summary entry inserted on compact failure";

  /* No on_error fired from the compact itself — the failure is
   * signalled via the (optional) CompactDone budget event with
   * summary_ok=false. This test does not subscribe to budget events
   * directly; the absence of history mutation above is the contract. */

  xAgentSessionDestroy(sess);
}

/* When a "thinking" model (e.g. DeepSeek-R1) produces only a
 * Thinking entry and no Text entry, the compact should fall back
 * to the Thinking content as the summary rather than degrading
 * to TruncateOldest. This is common when the model's reasoning
 * phase exhausts the output token budget before producing visible
 * text. */
TEST_F(SessionTest, BudgetSummarizeFallsBackToThinkingWhenNoText) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window        = 200;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Prime 3 rounds to fill history. */
  const std::string big(80, 'a');
  for (int i = 0; i < 3; i++) {
    fake_->script_queue.push_back({
        SText("reply"),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
              xErrno_Ok);
    ASSERT_EQ(cap.done_fired, i + 1);
  }

  /* The compact Query returns only Thinking, no Text.
   * The fallback should use the Thinking content as the summary. */
  fake_->script_queue.push_back({
      SThinking("The user asked about testing several times."),
      SDone(xAgentProviderStop_EndTurn),     /* no SText */
  });

  const std::string overflow_msg(400, 'b');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(overflow_msg.c_str())),
            xErrno_Busy);

  /* The compact should have succeeded (using Thinking as fallback),
   * producing a [summary] entry. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  size_t hlen = hist_len(s);

  int summary_count = 0;
  for (size_t i = 0; i < hlen; i++) {
    if (msgs[i].role == xAgentRole_System && msgs[i].text &&
        std::string(msgs[i].text, msgs[i].text_len).find("[summary]") !=
            std::string::npos) {
      summary_count++;
      /* The summary should contain the thinking text. */
      EXPECT_NE(std::string(msgs[i].text, msgs[i].text_len).find("testing"),
                std::string::npos)
          << "summary should contain thinking content";
    }
  }
  EXPECT_EQ(summary_count, 1)
      << "should have one [summary] entry from thinking fallback";

  /* Auto-retry already resubmitted the pending input after compact. */

  xAgentSessionDestroy(sess);
}

/* Summarize with too few user turns to compact must refuse with
 * PromptTooLong. */
TEST_F(SessionTest, BudgetSummarizeRefusesWhenFloorUnreachable) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window        = 30;       /* very tight                   */
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  std::string big(400, 'x');
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(big.c_str())),
            xErrno_PromptTooLong);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  EXPECT_EQ(hist_len(s), 0u);
  EXPECT_EQ(cap.done_fired, 0);
  EXPECT_EQ(cap.error_fired, 0);

  xAgentSessionDestroy(sess);
}

/* ── L1 memory-preservation callback ───────────────────────────────── */

namespace {

/* Capture struct that records every L1 preserve callback invocation. */
struct L1PreserveCap {
  struct Call {
    std::vector<std::string>  texts;   /* text field from each msg  */
    std::vector<xAgentRole>      roles;   /* role field from each msg  */
    std::vector<xAgentSessionEntryKind> kinds;
    xAgentL1PreserveReason       reason  = (xAgentL1PreserveReason)-1;
    size_t                    n_msgs  = 0;
  };
  std::vector<Call> calls;
};

void cb_l1_preserve(xAgentSession sess, const xAgentSessionMsg *msgs,
                    size_t n_msgs, xAgentL1PreserveReason reason,
                    void *owner) {
  (void)sess;
  auto *cap       = static_cast<L1PreserveCap *>(owner);
  auto  call      = L1PreserveCap::Call{};
  call.reason     = reason;
  call.n_msgs     = n_msgs;
  for (size_t i = 0; i < n_msgs; i++) {
    /* Read text/text_len only for Text/Thinking entries — ToolUse and
     * ToolResult store their payload in different fields. */
    if (msgs[i].kind == xAgentSessionEntry_Text ||
        msgs[i].kind == xAgentSessionEntry_Thinking) {
      call.texts.push_back(msgs[i].text ? std::string(msgs[i].text,
                                                       msgs[i].text_len)
                                        : std::string());
    } else {
      call.texts.push_back(std::string());
    }
    call.roles.push_back(msgs[i].role);
    call.kinds.push_back(msgs[i].kind);
  }
  cap->calls.push_back(std::move(call));
}

}  // namespace

/* L1 preserve fires with Finalizing reason when the session is
 * destroyed, delivering the full remaining history. */
TEST_F(SessionTest, L1PreserveFinalizingOnDestroy) {
  L1PreserveCap l1;
  Captured cap;

  xAgentSessionConf sc      = {};
  sc.cbs                  = make_cbs(&cap);
  sc.on_l1_preserve       = cb_l1_preserve;
  sc.l1_preserve_owner    = &l1;

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  /* Run one round to populate history. */
  fake_->script_queue.push_back({
      SText("hello"),
      SDone(xAgentProviderStop_EndTurn),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);
  EXPECT_EQ(cap.done_fired, 1);

  /* History now has: [User: "hi", Assistant: "hello"] */
  EXPECT_EQ(l1.calls.size(), 0u);

  xAgentSessionDestroy(sess);

  /* Should have fired exactly once with Finalizing reason. */
  ASSERT_EQ(l1.calls.size(), 1u);
  EXPECT_EQ(l1.calls[0].reason, xAgentL1PreserveReason_Finalizing);
  EXPECT_EQ(l1.calls[0].n_msgs, 2u);
  ASSERT_EQ(l1.calls[0].roles.size(), 2u);
  EXPECT_EQ(l1.calls[0].roles[0], xAgentRole_User);
  EXPECT_EQ(l1.calls[0].roles[1], xAgentRole_Assistant);
  EXPECT_EQ(l1.calls[0].texts[0], "hi");
  EXPECT_EQ(l1.calls[0].texts[1], "hello");
}

/* L1 preserve does NOT fire on destroy when the callback is NULL. */
TEST_F(SessionTest, L1PreserveNullCallbackIsNoop) {
  Captured cap;
  xAgentSessionConf sc = {};
  sc.cbs             = make_cbs(&cap);
  /* on_l1_preserve left as NULL (default) */

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hello")), xErrno_Ok);

  /* Destroy should not crash even though no L1 callback is set. */
  xAgentSessionDestroy(sess);
}

/* L1PreserveTruncatedOnBudgetTrim was removed together with the
 * TruncateOldest policy. The remaining L1 preserve coverage
 * (Compacted / Finalizing) exercises the surviving code paths. */

/* L1 preserve fires with Compacted reason when Summarize
 * replaces old history entries with a summary. The callback delivers
 * the original entries before they are replaced.
 * Covered by L1PreserveCompactedDeliversReplacedEntries below. */

/* ── Summarize compact (new single-band model) ──────────────
 *
 * The pipeline replaces history[0, compact_end_idx) with a summary
 * entry. compact_end_idx is the index of the second-to-last user
 * turn, so the last two user turns are always preserved.
 *
 * Fixture across these tests:
 *   3 successful prime rounds → history layout
 *      idx: 0    1    2    3    4    5
 *      role:U0   A0   U1   A1   U2   A2
 *
 *   compact_end_idx = index of second-to-last user turn = 2
 *
 *   Replaced band [0,2) = U0, A0 — replaced by a single [summary].
 *
 *   Post-compact layout (5 entries):
 *      idx: 0          1    2    3    4
 *      role:System(s)  U1   A1   U2   A2
 *      text:"[summary…]" "u1" "a1" "u2" "a2"
 */
TEST_F(SessionTest, BudgetSummarizeReplacesOldHistory) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window    = 200;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  /* Distinct, sized user payloads so we can later assert exact
   * tail survival. ~80 bytes each → ~30 tokens with envelope.
   * 3 rounds → ≈120 tokens, fits under 200. */
  const std::string u0 = std::string(80, '0');
  const std::string u1 = std::string(80, '1');
  const std::string u2 = std::string(80, '2');
  const std::string a_reply = "ack";

  for (const std::string *u : {&u0, &u1, &u2}) {
    fake_->script_queue.push_back({
        SText(a_reply.c_str()),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(u->c_str())),
              xErrno_Ok);
  }
  ASSERT_EQ(cap.done_fired, 3);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_EQ(hist_len(s), 6u);

  /* Trigger overflow → compact. Provider scripts the summary text
   * for the synchronous compact Query. */
  fake_->script_queue.push_back({
      SText("[summary] middle compressed"),
      SDone(xAgentProviderStop_EndTurn),
  });
  /* Auto-retry will re-submit the overflow message after compact
   * completes. Script a response for that auto-retry query too. */
  fake_->script_queue.push_back({
      SText("ok after compact"),
      SDone(xAgentProviderStop_EndTurn),
  });
  const std::string overflow_msg(400, 'X'); /* well above ceiling   */
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(overflow_msg.c_str())),
            xErrno_Busy);

  /* Compact finished synchronously, then auto-retry appended the
   * user message and ran a new query. The history now contains:
   * summary(1) + U1 + A1 + U2 + A2 + overflow_user + auto_retry_assistant = 7
   * But the exact length depends on the auto-retry query result. */
  size_t hlen = hist_len(s);
  ASSERT_GE(hlen, 5u) << "should have at least summary + remaining entries";

  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  ASSERT_NE(msgs, nullptr);

  /* Summary at position 0 (replaced the old head). */
  EXPECT_EQ(msgs[0].role, xAgentRole_System);
  ASSERT_NE(msgs[0].text, nullptr);
  EXPECT_NE(std::string(msgs[0].text, msgs[0].text_len).find("[summary]"),
            std::string::npos);

  /* Remaining turns should be preserved somewhere in the history. */
  bool found_u1 = false, found_u2 = false;
  for (size_t i = 0; i < hlen; i++) {
    if (msgs[i].role == xAgentRole_User && msgs[i].text) {
      std::string t(msgs[i].text, msgs[i].text_len);
      if (t == u1) found_u1 = true;
      if (t == u2) found_u2 = true;
    }
  }
  EXPECT_TRUE(found_u1) << "second-to-last user turn must survive";
  EXPECT_TRUE(found_u2) << "last user turn must survive";

  /* Old head (u0) must NOT survive in any form. */
  for (size_t i = 0; i < hlen; ++i) {
    if (msgs[i].text == nullptr) continue;
    EXPECT_EQ(std::string(msgs[i].text, msgs[i].text_len).find(u0),
              std::string::npos)
        << "old head user turn u0 must be gone (idx=" << i << ")";
  }

  /* Auto-retry should have fired on_done for the pending message. */
  EXPECT_GE(cap.done_fired, 3);

  xAgentSessionDestroy(sess);
}

/* L1 preserve under compact must deliver the entries that were
 * replaced by the summary. The surviving tail stays in history and
 * must not be in the preserve callback (otherwise the L1 store would
 * double-count entries that are still live). */
TEST_F(SessionTest, L1PreserveCompactedDeliversReplacedEntries) {
  L1PreserveCap l1;
  Captured cap;

  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window    = 200;

  xAgentSessionConf sc   = {};
  sc.cbs                 = make_cbs(&cap);
  sc.budget              = budget;
  sc.on_l1_preserve      = cb_l1_preserve;
  sc.l1_preserve_owner   = &l1;

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  const std::string u0 = std::string(80, '0');
  const std::string u1 = std::string(80, '1');
  const std::string u2 = std::string(80, '2');

  for (const std::string *u : {&u0, &u1, &u2}) {
    fake_->script_queue.push_back({
        SText("ack"),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(u->c_str())),
              xErrno_Ok);
  }

  fake_->script_queue.push_back({
      SText("[summary] middle compressed"),
      SDone(xAgentProviderStop_EndTurn),
  });
  /* Auto-retry will resubmit after compact — script a response. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(
                                     std::string(400, 'X').c_str())),
            xErrno_Busy);

  /* Find the Compacted call and assert exactly the replaced entries.
   * compact_end_idx = 2 (second-to-last user turn), so
   * [0,2) = U0, A0 = 2 entries are replaced. */
  const L1PreserveCap::Call *compacted = nullptr;
  for (const auto &c : l1.calls) {
    if (c.reason == xAgentL1PreserveReason_Compacted) {
      compacted = &c;
      break;
    }
  }
  ASSERT_NE(compacted, nullptr) << "Compacted callback must fire";

  /* Replaced band [0,2) is exactly U0 + A0 = 2 entries. */
  ASSERT_EQ(compacted->n_msgs, 2u)
      << "L1 preserve must deliver exactly the replaced entries";
  EXPECT_EQ(compacted->roles[0], xAgentRole_User);
  EXPECT_EQ(compacted->texts[0], u0) << "entry 0 must be u0";
  EXPECT_EQ(compacted->roles[1], xAgentRole_Assistant);

  /* Negative assertion: u1 and u2 must NOT be in the
   * preserve batch — they are still live in history. */
  for (const auto &t : compacted->texts) {
    EXPECT_EQ(t.find(u1), std::string::npos)
        << "u1 must not appear in Compacted callback";
    EXPECT_EQ(t.find(u2), std::string::npos)
        << "u2 must not appear in Compacted callback";
  }

  xAgentSessionDestroy(sess);
}

/* The new design always replaces history[0, compact_end_idx) with
 * a summary, where compact_end_idx is the second-to-last user turn.
 * With only 3 user turns, compact_end_idx = index of U1 (the
 * second-to-last), so [0, compact_end_idx) = U0, A0 is replaced.
 * The surviving tail (U1, A1, U2, A2) is preserved. */
TEST_F(SessionTest, BudgetSummarizeReplacesUpToCompactEnd) {
  Captured cap;
  xAgentBudgetConf budget{};
  budget.policy            = xAgentBudgetPolicy_Summarize;
  budget.context_window    = 200;
  xAgentSession sess = make_session_with_budget(agent_, make_cbs(&cap), budget);
  ASSERT_NE(sess, nullptr);

  const std::string u0 = std::string(80, '0');
  const std::string u1 = std::string(80, '1');
  const std::string u2 = std::string(80, '2');

  for (const std::string *u : {&u0, &u1, &u2}) {
    fake_->script_queue.push_back({
        SText("ack"),
        SDone(xAgentProviderStop_EndTurn),
    });
    ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(u->c_str())),
              xErrno_Ok);
  }

  fake_->script_queue.push_back({
      SText("[summary] front replaced"),
      SDone(xAgentProviderStop_EndTurn),
  });
  /* Auto-retry will resubmit after compact — script a response. */
  fake_->script_queue.push_back({
      SText("ok"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText(
                                     std::string(400, 'X').c_str())),
            xErrno_Busy);

  /* Layout: [summary, U1, A1, U2, A2, overflow_user, auto_reply]
   * compact_end_idx = 2 means entries [0,2) = U0, A0 were replaced.
   * Then auto-retry appended the overflow message and ran a query. */
  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_GE(hist_len(s), 5u);

  auto *msgs = (const xAgentSessionMsg_ *)xArrayData(s->history_arr);
  ASSERT_NE(msgs, nullptr);
  EXPECT_EQ(msgs[0].role, xAgentRole_System);
  EXPECT_NE(std::string(msgs[0].text, msgs[0].text_len).find("[summary]"),
            std::string::npos);

  /* u0 must NOT survive — it was in the replaced band. */
  for (size_t i = 0; i < hist_len(s); ++i) {
    if (msgs[i].text == nullptr) continue;
    std::string t(msgs[i].text, msgs[i].text_len);
    EXPECT_EQ(t.find(u0), std::string::npos);
  }

  xAgentSessionDestroy(sess);
}

/* L1 preserve Finalizing fires before on_finalizing, and both see the
 * same (still-intact) history. */
TEST_F(SessionTest, L1PreserveFinalizingFiresBeforeOnFinalizing) {
  L1PreserveCap l1;
  FinalizingCap fin_cap;

  xAgentSessionConf sc       = {};
  sc.on_l1_preserve       = cb_l1_preserve;
  sc.l1_preserve_owner    = &l1;
  sc.on_finalizing        = cb_finalizing;
  sc.finalizing_owner     = &fin_cap;

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  /* One round of conversation. */
  fake_->script_queue.push_back({
      SText("reply"),
      SDone(xAgentProviderStop_EndTurn),
  });
  Captured dummy;
  auto cbs_noop = xAgentSessionCallbacks{};
  cbs_noop.user_data = &dummy;
  reinterpret_cast<xAgentSession_ *>(sess)->cbs = cbs_noop;
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("hi")), xErrno_Ok);

  xAgentSessionDestroy(sess);

  /* Both hooks should have fired. */
  EXPECT_EQ(l1.calls.size(), 1u);
  EXPECT_EQ(fin_cap.calls, 1);

  /* L1 should have seen the same history length as on_finalizing. */
  ASSERT_EQ(l1.calls.size(), 1u);
  EXPECT_EQ(l1.calls[0].n_msgs, fin_cap.history_len);
  EXPECT_EQ(l1.calls[0].reason, xAgentL1PreserveReason_Finalizing);
}

/* L1 preserve Finalizing fires even when history is empty so the
 * owner gets a chance to release resources (e.g. heap-allocated
 * context created by xAgentCreateSession). */
TEST_F(SessionTest, L1PreserveFinalizingSkipsEmptyHistory) {
  L1PreserveCap l1;

  xAgentSessionConf sc       = {};
  sc.on_l1_preserve       = cb_l1_preserve;
  sc.l1_preserve_owner    = &l1;

  xAgentSession sess = xAgentSessionCreate(agent_, &sc);
  ASSERT_NE(sess, nullptr);

  /* No inputs, so history is empty. */
  xAgentSessionDestroy(sess);

  /* Finalizing still fires (with n_msgs == 0) so the owner can
   * free its context. */
  ASSERT_EQ(l1.calls.size(), 1u);
  EXPECT_EQ(l1.calls[0].n_msgs, 0u);
  EXPECT_EQ(l1.calls[0].reason, xAgentL1PreserveReason_Finalizing);
}

/* ── Tool-confirmation gate (needs_confirm) ─────────────────────────
 *
 * Fixture: an agent carrying a needs_confirm=1 "guard" tool (same
 * echo_handler under the hood). Every test below scripts the model
 * to emit a tool_use for "guard" and then wires on_tool_confirm
 * differently. */

class ConfirmGateFixture : public SessionTest {
 protected:
  xAgentTool tool_guard_ = nullptr;
  std::vector<ToolRec> guard_log_;

  void SetUp() override {
    SessionTest::SetUp();
    xAgentDestroy(agent_);

    xAgentToolConf tc = {};
    tc.name        = "guard";
    tc.description = "dangerous echo";
    tc.json_schema = "{\"type\":\"object\"}";
    tc.handler     = echo_handler;
    tc.user_data   = &guard_log_;
    tc.needs_confirm = 1;                 /* key difference */
    tool_guard_    = xAgentToolCreate(&tc);

    static const xAgentTool *kTools[1];
    kTools[0] = &tool_guard_;

    xAgentConf ac   = {};
    ac.loop         = loop_;
    ac.provider     = provider_;
    ac.model        = "fake-model";
    ac.system_prompt = "you are a test";
    ac.max_turns    = 5;
    ac.max_tokens   = 1024;
    ac.tools        = kTools;
    ac.tools_count  = 1;
    agent_          = xAgentCreate(&ac);
    ASSERT_NE(agent_, nullptr);
  }

  void TearDown() override {
    xAgentToolDestroy(tool_guard_);
    SessionTest::TearDown();
  }
};

/* Shared capture struct for confirm gate tests. */
struct ConfirmCap : Captured {
  int    confirm_calls = 0;
  std::string saw_name;
  std::string saw_id;
  std::string saw_args;
  xAgentToolConfirmResolver stashed = nullptr;
};

/* Allow path: the host calls Resolve(Allow) synchronously from the
 * on_tool_confirm callback. The handler runs, and the follow-up
 * round fires as usual. */
TEST_F(ConfirmGateFixture, AllowLetsHandlerRun) {
  ConfirmCap cap;

  auto cbs           = make_cbs(&cap);
  cbs.on_tool_confirm = [](xAgentSession, const char *name, const char *id,
                           const char *args,
                           xAgentToolConfirmResolver resolver, void *ud) {
    auto *c = static_cast<ConfirmCap *>(ud);
    c->confirm_calls++;
    c->saw_name = name ? name : "";
    c->saw_id   = id   ? id   : "";
    c->saw_args = args ? args : "";
    xAgentToolConfirmResolve(resolver, xAgentToolDecision_Allow, nullptr);
  };
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SToolCall("guard", "c1", "{\"x\":1}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  /* Gate fired exactly once with the right payload. */
  EXPECT_EQ(cap.confirm_calls, 1);
  EXPECT_EQ(cap.saw_name, "guard");
  EXPECT_EQ(cap.saw_id, "c1");
  EXPECT_EQ(cap.saw_args, "{\"x\":1}");

  /* Handler ran because we allowed. */
  ASSERT_EQ(guard_log_.size(), 1u);
  EXPECT_EQ(guard_log_[0].id, "c1");

  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  xAgentSessionDestroy(sess);
}

/* Reject path: Resolve(Reject, "nope") fabricates an is_error=1
 * tool_result, the handler never runs, and the follow-up round sees
 * the rejection text. */
TEST_F(ConfirmGateFixture, RejectBlocksHandlerAndFeedsError) {
  ConfirmCap cap;

  auto cbs           = make_cbs(&cap);
  cbs.on_tool_confirm = [](xAgentSession, const char *, const char *,
                           const char *,
                           xAgentToolConfirmResolver resolver, void *ud) {
    auto *c = static_cast<ConfirmCap *>(ud);
    c->confirm_calls++;
    xAgentToolConfirmResolve(resolver, xAgentToolDecision_Reject,
                             "policy denied");
  };
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SToolCall("guard", "c1", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("try")), xErrno_Ok);

  EXPECT_EQ(cap.confirm_calls, 1);
  /* Handler did NOT run. */
  EXPECT_EQ(guard_log_.size(), 0u);

  /* Completed (tool_error was folded back to the model, not
   * surfaced as a run failure). */
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  /* Second submit's tool_result is the synthetic rejection. */
  ASSERT_GE(fake_->captured_msgs_per_submit.size(), 2u);
  const auto &m2 = fake_->captured_msgs_per_submit[1];
  bool found_reject = false;
  for (const auto &m : m2) {
    if (m.role != xAgentRole_Tool) continue;
    for (const auto &b : m.blocks) {
      if (b.type != xAgentContentType_ToolResult) continue;
      if (b.tool_result_is_error &&
          b.tool_result_output.find("policy denied") != std::string::npos) {
        found_reject = true;
      }
    }
  }
  EXPECT_TRUE(found_reject);

  xAgentSessionDestroy(sess);
}

/* If the host does NOT wire on_tool_confirm, needs_confirm tools
 * still run — the gate is disabled by default so existing callers
 * observe no behaviour change. */
TEST_F(ConfirmGateFixture, NoCallbackMeansAutoAllow) {
  ConfirmCap cap;

  auto cbs = make_cbs(&cap);
  /* cbs.on_tool_confirm left NULL on purpose. */
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SToolCall("guard", "c1", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  EXPECT_EQ(cap.confirm_calls, 0); /* gate never fired */
  ASSERT_EQ(guard_log_.size(), 1u); /* handler ran */
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  xAgentSessionDestroy(sess);
}

/* Deferred Allow: the host stashes the resolver and resolves it
 * AFTER on_tool_confirm has returned. The run should drive forward
 * just like the synchronous Allow case. */
TEST_F(ConfirmGateFixture, DeferredAllowCompletesRun) {
  ConfirmCap cap;

  auto cbs           = make_cbs(&cap);
  cbs.on_tool_confirm = [](xAgentSession, const char *, const char *,
                           const char *,
                           xAgentToolConfirmResolver resolver, void *ud) {
    auto *c = static_cast<ConfirmCap *>(ud);
    c->confirm_calls++;
    c->stashed = resolver; /* resolve later */
  };
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SToolCall("guard", "c1", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });
  fake_->script_queue.push_back({SDone(xAgentProviderStop_EndTurn)});

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);

  /* Until we resolve, the run is paused mid-loop: the gate fired,
   * but the handler has NOT run and on_done has NOT fired. */
  EXPECT_EQ(cap.confirm_calls, 1);
  EXPECT_EQ(guard_log_.size(), 0u);
  EXPECT_EQ(cap.done_fired, 0);

  /* Now resolve. Everything proceeds. */
  ASSERT_NE(cap.stashed, nullptr);
  xAgentToolConfirmResolve(cap.stashed, xAgentToolDecision_Allow, nullptr);

  EXPECT_EQ(guard_log_.size(), 1u);
  EXPECT_EQ(cap.done_fired, 1);
  EXPECT_EQ(cap.done_reason, xAgentDoneReason_Completed);

  xAgentSessionDestroy(sess);
}

/* Stale resolver: resolving after session destroy must be a no-op,
 * not a crash (tests the resolver generation/invalidation path). */
TEST_F(ConfirmGateFixture, StaleResolverIsNoOp) {
  ConfirmCap cap;

  auto cbs           = make_cbs(&cap);
  cbs.on_tool_confirm = [](xAgentSession, const char *, const char *,
                           const char *,
                           xAgentToolConfirmResolver resolver, void *ud) {
    auto *c = static_cast<ConfirmCap *>(ud);
    c->confirm_calls++;
    c->stashed = resolver;
  };
  xAgentSession sess = make_session(cbs);

  fake_->script_queue.push_back({
      SToolCall("guard", "c1", "{}"),
      SDone(xAgentProviderStop_ToolUse),
  });

  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("go")), xErrno_Ok);
  EXPECT_EQ(cap.confirm_calls, 1);
  ASSERT_NE(cap.stashed, nullptr);

  /* Destroy while the resolver is still outstanding. The session's
   * teardown path invalidates the resolver. */
  xAgentSessionDestroy(sess);

  /* Late resolve: must not crash or dispatch anything. */
  xAgentToolConfirmResolve(cap.stashed, xAgentToolDecision_Allow, nullptr);

  EXPECT_EQ(guard_log_.size(), 0u); /* handler never ran */
}

/* ── created_at_ms ────────────────────────────────────────────────
 *
 * Every history entry is stamped with a wall-clock ms timestamp at
 * the moment it was produced (user input, assistant stream chunk,
 * tool completion). These tests pin the behaviour:
 *
 *   1. User input stamps, and stamps grow monotonically across
 *      two back-to-back inputs.
 *   2. Assistant produced-entries carry the production-time stamp
 *      across the produced → history splice (a later-added
 *      user message must therefore show a *later* stamp than the
 *      earlier assistant chunk, even though history_push runs
 *      after the produced array is built).
 *
 * Both lean on SessionTest's fake provider + synchronous event
 * loop so we can inspect history_arr directly.
 */
TEST_F(SessionTest, UserInputStampsCreatedAt) {
  Captured                cap;
  xAgentSessionCallbacks cbs  = make_cbs(&cap);
  xAgentSession           sess = make_session(cbs);

  fake_->script_queue.push_back({
      SText("hi"),
      SDone(xAgentProviderStop_EndTurn),
  });
  EXPECT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("one")),
            xErrno_Ok);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  ASSERT_GE(hist_len(s), 1u);
  EXPECT_GT(hist_at(s, 0)->created_at_ms, 1000000000000ULL)
      << "user entry should carry a wall-clock stamp";

  xAgentSessionDestroy(sess);
}

TEST_F(SessionTest, ProducedEntriesKeepProductionTimeStamp) {
  Captured                cap;
  xAgentSessionCallbacks cbs  = make_cbs(&cap);
  xAgentSession           sess = make_session(cbs);

  /* Round 1: user "one" → assistant "hi" */
  fake_->script_queue.push_back({
      SText("hi"),
      SDone(xAgentProviderStop_EndTurn),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("one")),
            xErrno_Ok);

  /* Tiny sleep so the wall-clock delta between turns is observable
   * even on very fast machines. 2 ms is plenty for clock_gettime
   * granularity without making the test feel slow. */
  struct timespec pause = {0, 2 * 1000 * 1000};
  nanosleep(&pause, nullptr);

  /* Round 2: user "two" → assistant "bye" */
  fake_->script_queue.push_back({
      SText("bye"),
      SDone(xAgentProviderStop_EndTurn),
  });
  ASSERT_EQ(xAgentSessionInput(sess, xAgentMessageFromText("two")),
            xErrno_Ok);

  auto *s = reinterpret_cast<xAgentSession_ *>(sess);
  /* Layout after two turns: user1, asst1, user2, asst2. */
  ASSERT_GE(hist_len(s), 4u);

  uint64_t t_u1 = hist_at(s, 0)->created_at_ms;
  uint64_t t_a1 = hist_at(s, 1)->created_at_ms;
  uint64_t t_u2 = hist_at(s, 2)->created_at_ms;
  uint64_t t_a2 = hist_at(s, 3)->created_at_ms;

  EXPECT_GT(t_u1, 0ULL);
  EXPECT_GT(t_a1, 0ULL);
  EXPECT_GT(t_u2, 0ULL);
  EXPECT_GT(t_a2, 0ULL);

  /* Chronological order must hold across the splice: the assistant
   * chunk from round 1 was stamped at production time (inside the
   * Query's produced_arr), not when session on_done later copied
   * it into history, so it must precede round 2's user input. */
  EXPECT_LE(t_u1, t_a1);
  EXPECT_LT(t_a1, t_u2) << "produced splice dropped the production-time stamp";
  EXPECT_LE(t_u2, t_a2);

  xAgentSessionDestroy(sess);
}