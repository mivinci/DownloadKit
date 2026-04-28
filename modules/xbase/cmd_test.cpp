/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cmd_test.cpp - Tests for xCommand async command executor
 */

#include <gtest/gtest.h>

#include <xbase/cmd.h>
#include <xbase/event.h>

#include <stdio.h>
#include <string.h>

/* ───────────────────── Helpers ───────────────────── */

struct TestCtx {
  xEventLoop  loop;
  xCommandResult  result;
  int         done;
  int         stdout_chunks;
  size_t      total_stdout;
};

static void on_done(xCommand, const xCommandResult *result, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->result = *result;
  ctx->done   = 1;
  xEventLoopStop(ctx->loop);
}

static void on_stdout_stream(xCommand, const char *, size_t len, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->stdout_chunks++;
  ctx->total_stdout += len;
}

/* Run the event loop until on_done fires (with a safety timeout). */
static void run_until_done(xEventLoop loop, struct TestCtx *ctx,
                           int timeout_ms = 10000) {
  uint64_t deadline = xMonoMs() + (uint64_t)timeout_ms;
  while (!ctx->done && (int64_t)(deadline - xMonoMs()) > 0) {
    xEventWait(loop, 100);
  }
}

/* ───────────────────── Capture mode ───────────────────── */

TEST(Command, CaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", "world", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.signaled, 0);
  EXPECT_EQ(ctx.result.timed_out, 0);
  EXPECT_GT(ctx.result.stdout_len, 0u);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_STREQ(ctx.result.stdout_buf, "hello world\n");
  EXPECT_EQ(ctx.result.stderr_len, 0u);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);
  EXPECT_GT(ctx.result.elapsed_ms, 0u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, CaptureBothStdoutStderr) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Capture;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_STREQ(ctx.result.stdout_buf, "out\n");
  EXPECT_STREQ(ctx.result.stderr_buf, "err\n");

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, NonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "exit 42", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, CommandNotFound) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  xCommandConf conf = {};
  conf.cmd          = "/nonexistent/command";
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 127);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Stream mode ───────────────────── */

TEST(Command, StreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"streaming", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Stream;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Discard mode ───────────────────── */

TEST(Command, DiscardAll) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"discarded", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Timeout ───────────────────── */

TEST(Command, Timeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.timeout_ms   = 200;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  uint64_t start = xMonoMs();
  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Cancel ───────────────────── */

TEST(Command, Cancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCommandCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Query ───────────────────── */

TEST(Command, QueryWhileRunning) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running */
  EXPECT_EQ(xCommandPid(exec), -1);
  EXPECT_EQ(xCommandIsRunning(exec), 0);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running */
  EXPECT_GT(xCommandPid(exec), 0);
  EXPECT_EQ(xCommandIsRunning(exec), 1);

  run_until_done(loop, &ctx, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Busy guard ───────────────────── */

TEST(Command, RunWhileBusy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Trying to run again while busy should fail */
  err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  EXPECT_EQ(err, xErrno_Busy);

  run_until_done(loop, &ctx, 5000);
  EXPECT_EQ(ctx.done, 1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Working directory ───────────────────── */

TEST(Command, WorkingDirectory) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  xCommandConf conf = {};
  conf.cmd          = "/bin/pwd";
  conf.cwd          = "/tmp";
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  /* On macOS /tmp is a symlink to /private/tmp */
  EXPECT_TRUE(strcmp(ctx.result.stdout_buf, "/tmp\n") == 0 ||
              strcmp(ctx.result.stdout_buf, "/private/tmp\n") == 0);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Sequential runs ───────────────────── */

TEST(Command, SequentialRuns) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Run 1 */
  struct TestCtx ctx1 = {};
  ctx1.loop = loop;

  const char *argv1[] = {"first", nullptr};
  xCommandConf conf1 = {};
  conf1.cmd          = "/bin/echo";
  conf1.argv         = argv1;
  conf1.stdout_mode  = xCommandOutput_Capture;
  conf1.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandRun(exec, &conf1, NULL, NULL, on_done, &ctx1);
  ASSERT_EQ(err, xErrno_Ok);
  run_until_done(loop, &ctx1);
  EXPECT_STREQ(ctx1.result.stdout_buf, "first\n");

  /* Run 2 — reuse the same executor */
  struct TestCtx ctx2 = {};
  ctx2.loop = loop;

  const char *argv2[] = {"second", nullptr};
  xCommandConf conf2 = {};
  conf2.cmd          = "/bin/echo";
  conf2.argv         = argv2;
  conf2.stdout_mode  = xCommandOutput_Capture;
  conf2.stderr_mode  = xCommandOutput_Discard;

  err = xCommandRun(exec, &conf2, NULL, NULL, on_done, &ctx2);
  ASSERT_EQ(err, xErrno_Ok);
  run_until_done(loop, &ctx2);
  EXPECT_STREQ(ctx2.result.stdout_buf, "second\n");

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Null safety ───────────────────── */

TEST(Command, NullArgs) {
  EXPECT_EQ(xCommandCreate(NULL), nullptr);
  xCommandDestroy(NULL); /* should not crash */
  EXPECT_EQ(xCommandRun(NULL, NULL, NULL, NULL, NULL, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCommandCancel(NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCommandPid(NULL), -1);
  EXPECT_EQ(xCommandIsRunning(NULL), 0);
  EXPECT_EQ(xCommandPtyFd(NULL), -1);
}

/* ───────────────────── PTY mode ───────────────────── */

TEST(Command, PtyCaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", "world", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.signaled, 0);
  EXPECT_GT(ctx.result.stdout_len, 0u);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  /* In PTY mode, echo output includes CR before LF */
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "hello world") != nullptr);
  /* PTY fd should be -1 after completion */
  EXPECT_EQ(ctx.result.pty_fd, -1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyStreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"streaming", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Stream;
  conf.stderr_mode  = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  /* No separate stdout_buf in Stream mode */
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyMergesStderr) {
  /* In PTY mode, stderr is merged into stdout through the PTY */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Capture; /* ignored in PTY mode */
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  /* Both stdout and stderr output should be in stdout_buf */
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "out") != nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "err") != nullptr);
  /* stderr_buf should be NULL in PTY mode */
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_len, 0u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyFdQuery) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running — should return -1 */
  EXPECT_EQ(xCommandPtyFd(exec), -1);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Discard;
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int pty_fd = xCommandPtyFd(exec);
  EXPECT_GE(pty_fd, 0);

  run_until_done(loop, &ctx, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyNonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  // Try /bin/bash first since /bin/sh (dash) may behave differently in PTY
  const char *shell = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
  const char *argv[] = {"-c", "exit 42", nullptr};
  xCommandConf conf = {};
  conf.cmd          = shell;
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  // Debug: print full result to diagnose CI flaky test (exit_code=1 instead of 42)
  fprintf(stderr, "[DEBUG PtyNonZeroExitCode] shell=%s exit_code=%d, signaled=%d, timed_out=%d, elapsed_ms=%lu\n",
          shell, ctx.result.exit_code, ctx.result.signaled, ctx.result.timed_out,
          (unsigned long)ctx.result.elapsed_ms);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.timeout_ms   = 200;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;
  conf.input_mode   = xCommandInput_Pty;

  uint64_t start = xMonoMs();
  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCommandCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyDiscardMode) {
  /* PTY with Discard mode: child gets a terminal but we don't read output */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommand exec = xCommandCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", nullptr};
  xCommandConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;
  conf.input_mode   = xCommandInput_Pty;

  xErrno err = xCommandRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCommandDestroy(exec);
  xEventLoopDestroy(loop);
}
