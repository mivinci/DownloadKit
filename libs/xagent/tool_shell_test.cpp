/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_shell_test.cpp - Unit tests for xAgentToolShellCreate / xAgentToolShellStdinCreate
 */

#include <gtest/gtest.h>

extern "C" {
#include <xagent/tool_shell.h>
#include <xagent/tool.h>
#include <xagent/message.h>
#include <xbase/event.h>
#include <xbase/error.h>
#include "tool_private.h"
}

#include <cstring>

/* ───────────────────── Create / Destroy ───────────────────── */

TEST(ShellTool, CreateWithDefaults) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(tool, nullptr);

  EXPECT_STREQ(ai_tool_name(tool), "shell");
  EXPECT_NE(ai_tool_description(tool), nullptr);
  EXPECT_NE(ai_tool_json_schema(tool), nullptr);
  EXPECT_EQ(ai_tool_concurrent_safe(tool), 1);
  EXPECT_EQ(ai_tool_needs_confirm(tool), 1);

  xAgentToolDestroy(tool);
  xEventLoopDestroy(loop);
}

TEST(ShellTool, CreateWithConf) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentShellConf conf = {};
  conf.timeout_ms  = 5000;
  conf.stdout_cap  = 1024;
  conf.stderr_cap  = 1024;

  xAgentTool tool = xAgentToolShellCreate(loop, &conf);
  ASSERT_NE(tool, nullptr);

  xAgentToolDestroy(tool);
  xEventLoopDestroy(loop);
}

TEST(ShellTool, CreateNullLoop) {
  EXPECT_EQ(xAgentToolShellCreate(nullptr, nullptr), nullptr);
}

TEST(ShellTool, DestroyNullIsNoop) {
  xAgentToolDestroy(nullptr);
}

TEST(ShellTool, UserDataIsShellCtx) {
  /* The tool's user_data should be a non-NULL ShellCtx pointer */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(tool, nullptr);

  EXPECT_NE(xAgentToolUserData(tool), nullptr);

  xAgentToolDestroy(tool);
  xEventLoopDestroy(loop);
}

TEST(ShellTool, OnCancelIsSet) {
  /* Shell tool should have an on_cancel_fn that sends SIGTERM */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(tool, nullptr);

  EXPECT_NE(ai_tool_on_cancel_fn(tool), nullptr);

  xAgentToolDestroy(tool);
  xEventLoopDestroy(loop);
}

/* ───────────────────── shell_stdin tool ───────────────────── */

TEST(ShellStdinTool, CreateFromShellTool) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin_tool = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin_tool, nullptr);

  EXPECT_STREQ(ai_tool_name(stdin_tool), "shell_stdin");
  EXPECT_NE(ai_tool_description(stdin_tool), nullptr);
  EXPECT_NE(ai_tool_json_schema(stdin_tool), nullptr);
  EXPECT_EQ(ai_tool_concurrent_safe(stdin_tool), 1);
  EXPECT_EQ(ai_tool_needs_confirm(stdin_tool), 0);

  /* stdin_tool does NOT own user_data (shell_tool does) */
  EXPECT_NE(xAgentToolUserData(stdin_tool), nullptr);
  /* Both tools share the same ShellCtx */
  EXPECT_EQ(xAgentToolUserData(stdin_tool), xAgentToolUserData(shell_tool));

  xAgentToolDestroy(stdin_tool);
  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}

TEST(ShellStdinTool, CreateNullShellTool) {
  EXPECT_EQ(xAgentToolShellStdinCreate(nullptr), nullptr);
}

TEST(ShellStdinTool, DestroyOrderIndependent) {
  /* Destroying stdin_tool before shell_tool should be safe */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin_tool = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin_tool, nullptr);

  /* Destroy stdin_tool first — it does NOT own user_data */
  xAgentToolDestroy(stdin_tool);
  /* shell_tool should still be valid */
  EXPECT_NE(xAgentToolUserData(shell_tool), nullptr);

  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}

TEST(ShellStdinTool, HandlerNoRunningCommand) {
  /* Calling shell_stdin when no command is running should return
   * an error result (not crash). */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin_tool = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin_tool, nullptr);

  /* Invoke the stdin handler with a tool_use for a nonexistent command */
  xAgentContent in = {};
  in.type                   = xAgentContentType_ToolUse;
  in.u.tool_use.id          = "call_stdin_1";
  in.u.tool_use.name        = "shell_stdin";
  in.u.tool_use.args_json   = R"({"input":"hello\n","tool_use_id":"call_shell_1"})";

  xAgentContent out = {};
  xErrno err = ai_tool_invoke(stdin_tool, nullptr, &in, &out);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(out.type, xAgentContentType_ToolResult);
  EXPECT_NE(out.u.tool_result.output, nullptr);
  EXPECT_EQ(out.u.tool_result.is_error, 1);
  /* Output should mention "No running shell command" */
  EXPECT_NE(strstr(out.u.tool_result.output, "No running"), nullptr);

  xAgentToolDestroy(stdin_tool);
  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}

TEST(ShellStdinTool, HandlerInvalidArgs) {
  /* Various invalid args_json cases should return xErrno_InvalidArg */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin_tool = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin_tool, nullptr);

  /* NULL args_json */
  xAgentContent in1 = {};
  in1.type                 = xAgentContentType_ToolUse;
  in1.u.tool_use.id        = "call_1";
  in1.u.tool_use.name      = "shell_stdin";
  in1.u.tool_use.args_json = nullptr;

  xAgentContent out1 = {};
  EXPECT_EQ(ai_tool_invoke(stdin_tool, nullptr, &in1, &out1), xErrno_InvalidArg);

  /* Missing required fields */
  xAgentContent in2 = {};
  in2.type                 = xAgentContentType_ToolUse;
  in2.u.tool_use.id        = "call_2";
  in2.u.tool_use.name      = "shell_stdin";
  in2.u.tool_use.args_json = R"({"input":"hello"})";

  xAgentContent out2 = {};
  EXPECT_EQ(ai_tool_invoke(stdin_tool, nullptr, &in2, &out2), xErrno_InvalidArg);

  /* Empty input string */
  xAgentContent in3 = {};
  in3.type                 = xAgentContentType_ToolUse;
  in3.u.tool_use.id        = "call_3";
  in3.u.tool_use.name      = "shell_stdin";
  in3.u.tool_use.args_json = R"({"input":"","tool_use_id":""})";

  xAgentContent out3 = {};
  EXPECT_EQ(ai_tool_invoke(stdin_tool, nullptr, &in3, &out3), xErrno_InvalidArg);

  /* Malformed JSON */
  xAgentContent in4 = {};
  in4.type                 = xAgentContentType_ToolUse;
  in4.u.tool_use.id        = "call_4";
  in4.u.tool_use.name      = "shell_stdin";
  in4.u.tool_use.args_json = "not json";

  xAgentContent out4 = {};
  EXPECT_EQ(ai_tool_invoke(stdin_tool, nullptr, &in4, &out4), xErrno_InvalidArg);

  xAgentToolDestroy(stdin_tool);
  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Context sharing ───────────────────── */

TEST(ShellIntegration, ShellAndStdinShareContext) {
  /* Verify that both tools share the same ShellCtx, which is
   * required for shell_stdin to find running commands. */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin_tool = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin_tool, nullptr);

  void *shell_ud = xAgentToolUserData(shell_tool);
  void *stdin_ud = xAgentToolUserData(stdin_tool);

  EXPECT_NE(shell_ud, nullptr);
  EXPECT_EQ(shell_ud, stdin_ud); /* Same ShellCtx */

  xAgentToolDestroy(stdin_tool);
  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}

TEST(ShellIntegration, MultipleStdinToolsShareContext) {
  /* Multiple shell_stdin tools from the same shell_tool
   * should all share the same ShellCtx. */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xAgentTool shell_tool = xAgentToolShellCreate(loop, nullptr);
  ASSERT_NE(shell_tool, nullptr);

  xAgentTool stdin1 = xAgentToolShellStdinCreate(shell_tool);
  xAgentTool stdin2 = xAgentToolShellStdinCreate(shell_tool);
  ASSERT_NE(stdin1, nullptr);
  ASSERT_NE(stdin2, nullptr);

  EXPECT_EQ(xAgentToolUserData(stdin1), xAgentToolUserData(shell_tool));
  EXPECT_EQ(xAgentToolUserData(stdin2), xAgentToolUserData(shell_tool));

  xAgentToolDestroy(stdin2);
  xAgentToolDestroy(stdin1);
  xAgentToolDestroy(shell_tool);
  xEventLoopDestroy(loop);
}
