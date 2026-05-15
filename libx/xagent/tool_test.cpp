/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_test.cpp - Unit tests for xai/tool.{h,c}
 *
 * We reach into the module-internal ai_tool_* accessors via the
 * private header (tool_private.h). That keeps the tests honest
 * about the contract the provider layer actually consumes.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xagent/message.h>
#include <xagent/tool.h>
#include <xbase/error.h>
#include "tool_private.h"
}

#include <cstring>
#include <string>

/* ── A trivial handler used by several tests ──────────────────────────── */

struct HandlerSpy {
  int         calls     = 0;
  std::string last_args;
  std::string last_id;
  xErrno      ret       = xErrno_Ok;
  std::string out_str;
};

static xErrno spy_handler(xAgentQuery, const xAgentContent *in, xAgentContent *out, void *ud) {
  auto *spy = static_cast<HandlerSpy *>(ud);
  spy->calls++;
  if (in && in->type == xAgentContentType_ToolUse) {
    spy->last_args = in->u.tool_use.args_json ? in->u.tool_use.args_json : "";
    spy->last_id   = in->u.tool_use.id ? in->u.tool_use.id : "";
  }
  if (out) {
    memset(out, 0, sizeof(*out));
    out->type                 = xAgentContentType_ToolResult;
    out->u.tool_result.id     = spy->last_id.c_str();
    out->u.tool_result.output = spy->out_str.c_str();
    out->u.tool_result.output_len = spy->out_str.size();
  }
  return spy->ret;
}

/* ── Create / Destroy ─────────────────────────────────────────────────── */

TEST(XaiTool, CreateFullConfig) {
  HandlerSpy   spy;
  xAgentToolConf  conf = {};
  conf.name         = "echo";
  conf.description  = "Echo the input.";
  conf.json_schema  = "{\"type\":\"object\"}";
  conf.handler      = spy_handler;
  conf.user_data    = &spy;
  conf.concurrent_safe = 1;
  conf.needs_confirm   = 0;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_STREQ(ai_tool_name(t), "echo");
  EXPECT_STREQ(ai_tool_description(t), "Echo the input.");
  EXPECT_STREQ(ai_tool_json_schema(t), "{\"type\":\"object\"}");
  EXPECT_EQ(ai_tool_concurrent_safe(t), 1);
  EXPECT_EQ(ai_tool_needs_confirm(t), 0);

  xAgentToolDestroy(t);
}

TEST(XaiTool, CreateMinimalConfig) {
  HandlerSpy  spy;
  xAgentToolConf conf = {};
  conf.name    = "noop";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_STREQ(ai_tool_name(t), "noop");
  EXPECT_EQ(ai_tool_description(t), nullptr);
  EXPECT_EQ(ai_tool_json_schema(t), nullptr);
  EXPECT_EQ(ai_tool_concurrent_safe(t), 0);
  EXPECT_EQ(ai_tool_needs_confirm(t), 0);

  xAgentToolDestroy(t);
}

TEST(XaiTool, CreateNullConf) {
  EXPECT_EQ(xAgentToolCreate(nullptr), nullptr);
}

TEST(XaiTool, CreateMissingName) {
  HandlerSpy  spy;
  xAgentToolConf conf = {};
  conf.name    = nullptr;
  conf.handler = spy_handler;
  conf.user_data = &spy;
  EXPECT_EQ(xAgentToolCreate(&conf), nullptr);
}

TEST(XaiTool, CreateMissingHandler) {
  xAgentToolConf conf = {};
  conf.name    = "x";
  conf.handler = nullptr;
  EXPECT_EQ(xAgentToolCreate(&conf), nullptr);
}

TEST(XaiTool, ConfigFieldsAreSnapshotted) {
  /* Tool must own its own copies — caller's buffers disappearing
   * afterwards must not break anything. */
  HandlerSpy spy;

  char name[]        = "mutable_name";
  char description[] = "mutable_desc";
  char schema[]      = "{\"type\":\"object\"}";

  xAgentToolConf conf = {};
  conf.name         = name;
  conf.description  = description;
  conf.json_schema  = schema;
  conf.handler      = spy_handler;
  conf.user_data    = &spy;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  /* Scribble over the caller-side buffers. */
  memset(name,        0, sizeof(name));
  memset(description, 0, sizeof(description));
  memset(schema,      0, sizeof(schema));

  EXPECT_STREQ(ai_tool_name(t), "mutable_name");
  EXPECT_STREQ(ai_tool_description(t), "mutable_desc");
  EXPECT_STREQ(ai_tool_json_schema(t), "{\"type\":\"object\"}");

  xAgentToolDestroy(t);
}

TEST(XaiTool, DestroyNullIsNoop) {
  xAgentToolDestroy(nullptr); /* must not crash */
}

/* ── ai_tool_* accessors on NULL handle ───────────────────────────────── */

TEST(XaiTool, AccessorsOnNullHandleReturnDefaults) {
  EXPECT_EQ(ai_tool_name(nullptr), nullptr);
  EXPECT_EQ(ai_tool_description(nullptr), nullptr);
  EXPECT_EQ(ai_tool_json_schema(nullptr), nullptr);
  EXPECT_EQ(ai_tool_concurrent_safe(nullptr), 0);
  EXPECT_EQ(ai_tool_needs_confirm(nullptr), 0);
}

/* ── ai_tool_invoke ───────────────────────────────────────────────────── */

TEST(XaiTool, InvokeDispatchesToHandler) {
  HandlerSpy spy;
  spy.out_str = R"({"result":42})";

  xAgentToolConf conf = {};
  conf.name    = "answer";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAgentContent in = {};
  in.type                   = xAgentContentType_ToolUse;
  in.u.tool_use.id          = "call_42";
  in.u.tool_use.name        = "answer";
  in.u.tool_use.args_json   = R"({"q":"life"})";

  xAgentContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_Ok);
  EXPECT_EQ(spy.calls, 1);
  EXPECT_EQ(spy.last_args, R"({"q":"life"})");
  EXPECT_EQ(spy.last_id,   "call_42");
  EXPECT_EQ(out.type,      xAgentContentType_ToolResult);
  EXPECT_STREQ(out.u.tool_result.output, R"({"result":42})");

  xAgentToolDestroy(t);
}

TEST(XaiTool, InvokePropagatesHandlerError) {
  HandlerSpy spy;
  spy.ret = xErrno_InvalidArg;

  xAgentToolConf conf = {};
  conf.name    = "fails";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAgentContent in  = {};
  in.type = xAgentContentType_ToolUse;
  xAgentContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_InvalidArg);
  EXPECT_EQ(spy.calls, 1);

  xAgentToolDestroy(t);
}

TEST(XaiTool, InvokeOnNullHandle) {
  xAgentContent in = {};
  xAgentContent out = {};
  EXPECT_EQ(ai_tool_invoke(nullptr, nullptr, &in, &out), xErrno_InvalidArg);
}

/* ── on_done_fn / on_done_ud accessors ───────────────────────────── */

static void dummy_on_done(xAgentQuery, const char *, xAgentTool, const xAgentContent *,
                          xAgentContent *, void *) {}

TEST(XaiTool, OnDoneFieldsAreCaptured) {
  HandlerSpy spy;
  int        marker = 42;

  xAgentToolConf conf = {};
  conf.name        = "async_echo";
  conf.handler     = spy_handler;
  conf.user_data   = &spy;
  conf.on_done_fn  = dummy_on_done;
  conf.on_done_ud  = &marker;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_done_fn(t), dummy_on_done);
  EXPECT_EQ(ai_tool_on_done_ud(t), &marker);

  xAgentToolDestroy(t);
}

TEST(XaiTool, OnDoneDefaultsToNull) {
  HandlerSpy spy;

  xAgentToolConf conf = {};
  conf.name    = "sync_echo";
  conf.handler = spy_handler;
  conf.user_data = &spy;
  /* on_done_fn and on_done_ud intentionally left zeroed. */

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_done_fn(t), nullptr);
  EXPECT_EQ(ai_tool_on_done_ud(t), nullptr);

  xAgentToolDestroy(t);
}

TEST(XaiTool, OnDoneAccessorsOnNullHandle) {
  EXPECT_EQ(ai_tool_on_done_fn(nullptr), nullptr);
  EXPECT_EQ(ai_tool_on_done_ud(nullptr), nullptr);
}

/* ── xErrno_Pending from handler ────────────────────────────────── */

TEST(XaiTool, InvokeReturnsPendingWhenHandlerSignalsAsync) {
  HandlerSpy spy;
  spy.ret = xErrno_Pending;

  xAgentToolConf conf = {};
  conf.name        = "async_op";
  conf.handler     = spy_handler;
  conf.user_data   = &spy;
  conf.on_done_fn  = dummy_on_done;
  conf.on_done_ud  = nullptr;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAgentContent in  = {};
  in.type               = xAgentContentType_ToolUse;
  in.u.tool_use.id      = "call_async_1";
  in.u.tool_use.name    = "async_op";
  in.u.tool_use.args_json = "{\"delay\":5}";

  xAgentContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_Pending);
  EXPECT_EQ(spy.calls, 1);
  EXPECT_EQ(spy.last_id, "call_async_1");
  /* out is NOT populated — caller must not read it. */

  xAgentToolDestroy(t);
}

/* ── on_cancel_fn / on_cancel_ud accessors ────────────────────────── */

static void dummy_on_cancel(xAgentQuery, const char *, xAgentTool, void *) {}

TEST(XaiTool, OnCancelFieldsAreCaptured) {
  HandlerSpy spy;
  int        marker = 99;

  xAgentToolConf conf = {};
  conf.name          = "cancellable";
  conf.handler       = spy_handler;
  conf.user_data     = &spy;
  conf.on_cancel_fn  = dummy_on_cancel;
  conf.on_cancel_ud  = &marker;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_cancel_fn(t), dummy_on_cancel);
  EXPECT_EQ(ai_tool_on_cancel_ud(t), &marker);

  xAgentToolDestroy(t);
}

TEST(XaiTool, OnCancelDefaultsToNull) {
  HandlerSpy spy;

  xAgentToolConf conf = {};
  conf.name      = "no_cancel";
  conf.handler   = spy_handler;
  conf.user_data = &spy;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_cancel_fn(t), nullptr);
  EXPECT_EQ(ai_tool_on_cancel_ud(t), nullptr);

  xAgentToolDestroy(t);
}

TEST(XaiTool, OnCancelAccessorsOnNullHandle) {
  EXPECT_EQ(ai_tool_on_cancel_fn(nullptr), nullptr);
  EXPECT_EQ(ai_tool_on_cancel_ud(nullptr), nullptr);
}

/* ── xAgentToolUserData ─────────────────────────────────────────────── */

TEST(XaiTool, UserDataReturnsPointer) {
  HandlerSpy spy;
  int        marker = 42;

  xAgentToolConf conf = {};
  conf.name      = "ud_test";
  conf.handler   = spy_handler;
  conf.user_data = &marker;

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xAgentToolUserData(t), &marker);

  xAgentToolDestroy(t);
}

TEST(XaiTool, UserDataNullWhenNotSet) {
  HandlerSpy spy;

  xAgentToolConf conf = {};
  conf.name      = "no_ud";
  conf.handler   = spy_handler;
  /* user_data intentionally left as nullptr (zero-init) */

  xAgentTool t = xAgentToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xAgentToolUserData(t), nullptr);

  xAgentToolDestroy(t);
}

TEST(XaiTool, UserDataNullHandle) {
  EXPECT_EQ(xAgentToolUserData(nullptr), nullptr);
}
