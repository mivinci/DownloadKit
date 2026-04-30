/*
 * Copyright 2025 The xKit Authors. All rights reserved.
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
#include <xai/message.h>
#include <xai/tool.h>
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

static xErrno spy_handler(xAiQuery, const xAiContent *in, xAiContent *out, void *ud) {
  auto *spy = static_cast<HandlerSpy *>(ud);
  spy->calls++;
  if (in && in->type == xAiContentType_ToolUse) {
    spy->last_args = in->u.tool_use.args_json ? in->u.tool_use.args_json : "";
    spy->last_id   = in->u.tool_use.id ? in->u.tool_use.id : "";
  }
  if (out) {
    memset(out, 0, sizeof(*out));
    out->type                 = xAiContentType_ToolResult;
    out->u.tool_result.id     = spy->last_id.c_str();
    out->u.tool_result.output = spy->out_str.c_str();
    out->u.tool_result.output_len = spy->out_str.size();
  }
  return spy->ret;
}

/* ── Create / Destroy ─────────────────────────────────────────────────── */

TEST(XaiTool, CreateFullConfig) {
  HandlerSpy   spy;
  xAiToolConf  conf = {};
  conf.name         = "echo";
  conf.description  = "Echo the input.";
  conf.json_schema  = "{\"type\":\"object\"}";
  conf.handler      = spy_handler;
  conf.user_data    = &spy;
  conf.concurrent_safe = 1;
  conf.needs_confirm   = 0;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_STREQ(ai_tool_name(t), "echo");
  EXPECT_STREQ(ai_tool_description(t), "Echo the input.");
  EXPECT_STREQ(ai_tool_json_schema(t), "{\"type\":\"object\"}");
  EXPECT_EQ(ai_tool_concurrent_safe(t), 1);
  EXPECT_EQ(ai_tool_needs_confirm(t), 0);

  xAiToolDestroy(t);
}

TEST(XaiTool, CreateMinimalConfig) {
  HandlerSpy  spy;
  xAiToolConf conf = {};
  conf.name    = "noop";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_STREQ(ai_tool_name(t), "noop");
  EXPECT_EQ(ai_tool_description(t), nullptr);
  EXPECT_EQ(ai_tool_json_schema(t), nullptr);
  EXPECT_EQ(ai_tool_concurrent_safe(t), 0);
  EXPECT_EQ(ai_tool_needs_confirm(t), 0);

  xAiToolDestroy(t);
}

TEST(XaiTool, CreateNullConf) {
  EXPECT_EQ(xAiToolCreate(nullptr), nullptr);
}

TEST(XaiTool, CreateMissingName) {
  HandlerSpy  spy;
  xAiToolConf conf = {};
  conf.name    = nullptr;
  conf.handler = spy_handler;
  conf.user_data = &spy;
  EXPECT_EQ(xAiToolCreate(&conf), nullptr);
}

TEST(XaiTool, CreateMissingHandler) {
  xAiToolConf conf = {};
  conf.name    = "x";
  conf.handler = nullptr;
  EXPECT_EQ(xAiToolCreate(&conf), nullptr);
}

TEST(XaiTool, ConfigFieldsAreSnapshotted) {
  /* Tool must own its own copies — caller's buffers disappearing
   * afterwards must not break anything. */
  HandlerSpy spy;

  char name[]        = "mutable_name";
  char description[] = "mutable_desc";
  char schema[]      = "{\"type\":\"object\"}";

  xAiToolConf conf = {};
  conf.name         = name;
  conf.description  = description;
  conf.json_schema  = schema;
  conf.handler      = spy_handler;
  conf.user_data    = &spy;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  /* Scribble over the caller-side buffers. */
  memset(name,        0, sizeof(name));
  memset(description, 0, sizeof(description));
  memset(schema,      0, sizeof(schema));

  EXPECT_STREQ(ai_tool_name(t), "mutable_name");
  EXPECT_STREQ(ai_tool_description(t), "mutable_desc");
  EXPECT_STREQ(ai_tool_json_schema(t), "{\"type\":\"object\"}");

  xAiToolDestroy(t);
}

TEST(XaiTool, DestroyNullIsNoop) {
  xAiToolDestroy(nullptr); /* must not crash */
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

  xAiToolConf conf = {};
  conf.name    = "answer";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAiContent in = {};
  in.type                   = xAiContentType_ToolUse;
  in.u.tool_use.id          = "call_42";
  in.u.tool_use.name        = "answer";
  in.u.tool_use.args_json   = R"({"q":"life"})";

  xAiContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_Ok);
  EXPECT_EQ(spy.calls, 1);
  EXPECT_EQ(spy.last_args, R"({"q":"life"})");
  EXPECT_EQ(spy.last_id,   "call_42");
  EXPECT_EQ(out.type,      xAiContentType_ToolResult);
  EXPECT_STREQ(out.u.tool_result.output, R"({"result":42})");

  xAiToolDestroy(t);
}

TEST(XaiTool, InvokePropagatesHandlerError) {
  HandlerSpy spy;
  spy.ret = xErrno_InvalidArg;

  xAiToolConf conf = {};
  conf.name    = "fails";
  conf.handler = spy_handler;
  conf.user_data = &spy;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAiContent in  = {};
  in.type = xAiContentType_ToolUse;
  xAiContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_InvalidArg);
  EXPECT_EQ(spy.calls, 1);

  xAiToolDestroy(t);
}

TEST(XaiTool, InvokeOnNullHandle) {
  xAiContent in = {};
  xAiContent out = {};
  EXPECT_EQ(ai_tool_invoke(nullptr, nullptr, &in, &out), xErrno_InvalidArg);
}

/* ── on_done_fn / on_done_ud accessors ───────────────────────────── */

static void dummy_on_done(xAiQuery, const char *, xAiTool, const xAiContent *,
                          xAiContent *, void *) {}

TEST(XaiTool, OnDoneFieldsAreCaptured) {
  HandlerSpy spy;
  int        marker = 42;

  xAiToolConf conf = {};
  conf.name        = "async_echo";
  conf.handler     = spy_handler;
  conf.user_data   = &spy;
  conf.on_done_fn  = dummy_on_done;
  conf.on_done_ud  = &marker;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_done_fn(t), dummy_on_done);
  EXPECT_EQ(ai_tool_on_done_ud(t), &marker);

  xAiToolDestroy(t);
}

TEST(XaiTool, OnDoneDefaultsToNull) {
  HandlerSpy spy;

  xAiToolConf conf = {};
  conf.name    = "sync_echo";
  conf.handler = spy_handler;
  conf.user_data = &spy;
  /* on_done_fn and on_done_ud intentionally left zeroed. */

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_done_fn(t), nullptr);
  EXPECT_EQ(ai_tool_on_done_ud(t), nullptr);

  xAiToolDestroy(t);
}

TEST(XaiTool, OnDoneAccessorsOnNullHandle) {
  EXPECT_EQ(ai_tool_on_done_fn(nullptr), nullptr);
  EXPECT_EQ(ai_tool_on_done_ud(nullptr), nullptr);
}

/* ── xErrno_Pending from handler ────────────────────────────────── */

TEST(XaiTool, InvokeReturnsPendingWhenHandlerSignalsAsync) {
  HandlerSpy spy;
  spy.ret = xErrno_Pending;

  xAiToolConf conf = {};
  conf.name        = "async_op";
  conf.handler     = spy_handler;
  conf.user_data   = &spy;
  conf.on_done_fn  = dummy_on_done;
  conf.on_done_ud  = nullptr;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  xAiContent in  = {};
  in.type               = xAiContentType_ToolUse;
  in.u.tool_use.id      = "call_async_1";
  in.u.tool_use.name    = "async_op";
  in.u.tool_use.args_json = "{\"delay\":5}";

  xAiContent out = {};
  EXPECT_EQ(ai_tool_invoke(t, nullptr, &in, &out), xErrno_Pending);
  EXPECT_EQ(spy.calls, 1);
  EXPECT_EQ(spy.last_id, "call_async_1");
  /* out is NOT populated — caller must not read it. */

  xAiToolDestroy(t);
}

/* ── on_cancel_fn / on_cancel_ud accessors ────────────────────────── */

static void dummy_on_cancel(xAiQuery, const char *, xAiTool, void *) {}

TEST(XaiTool, OnCancelFieldsAreCaptured) {
  HandlerSpy spy;
  int        marker = 99;

  xAiToolConf conf = {};
  conf.name          = "cancellable";
  conf.handler       = spy_handler;
  conf.user_data     = &spy;
  conf.on_cancel_fn  = dummy_on_cancel;
  conf.on_cancel_ud  = &marker;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_cancel_fn(t), dummy_on_cancel);
  EXPECT_EQ(ai_tool_on_cancel_ud(t), &marker);

  xAiToolDestroy(t);
}

TEST(XaiTool, OnCancelDefaultsToNull) {
  HandlerSpy spy;

  xAiToolConf conf = {};
  conf.name      = "no_cancel";
  conf.handler   = spy_handler;
  conf.user_data = &spy;

  xAiTool t = xAiToolCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(ai_tool_on_cancel_fn(t), nullptr);
  EXPECT_EQ(ai_tool_on_cancel_ud(t), nullptr);

  xAiToolDestroy(t);
}

TEST(XaiTool, OnCancelAccessorsOnNullHandle) {
  EXPECT_EQ(ai_tool_on_cancel_fn(nullptr), nullptr);
  EXPECT_EQ(ai_tool_on_cancel_ud(nullptr), nullptr);
}
