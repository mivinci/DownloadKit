/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cmd_test.cpp - Tests for xCmd async command executor
 */

#include <gtest/gtest.h>

#include <xbase/cmd.h>
#include <xbase/event.h>

#include <stdio.h>
#include <string.h>

/* ───────────────────── Helpers ───────────────────── */

struct TestCtx {
  xEventLoop  loop;
  xCmdResult  result;
  int         done;
  int         stdout_chunks;
  int         stderr_chunks;
  size_t      total_stdout;
  size_t      total_stderr;
};

static void on_done(xCmd, const xCmdResult *result, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->result = *result;
  ctx->done   = 1;
  xEventLoopStop(ctx->loop);
}

static void on_stdout_stream(xCmd, const char *, size_t len, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->stdout_chunks++;
  ctx->total_stdout += len;
}

static void on_stderr_stream(xCmd, const char *, size_t len, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->stderr_chunks++;
  ctx->total_stderr += len;
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

TEST(Cmd, CaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", "world", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
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

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, CaptureBothStdoutStderr) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Capture;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_STREQ(ctx.result.stdout_buf, "out\n");
  EXPECT_STREQ(ctx.result.stderr_buf, "err\n");

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, NonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "exit 42", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, CommandNotFound) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  xCmdConf conf = {};
  conf.cmd          = "/nonexistent/command";
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 127);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Stream mode ───────────────────── */

TEST(Cmd, StreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"streaming", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Stream;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Discard mode ───────────────────── */

TEST(Cmd, DiscardAll) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"discarded", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Timeout ───────────────────── */

TEST(Cmd, Timeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.timeout_ms   = 200;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  uint64_t start = xMonoMs();
  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Cancel ───────────────────── */

TEST(Cmd, Cancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCmdCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Query ───────────────────── */

TEST(Cmd, QueryWhileRunning) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running */
  EXPECT_EQ(xCmdPid(exec), -1);
  EXPECT_EQ(xCmdIsRunning(exec), 0);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running */
  EXPECT_GT(xCmdPid(exec), 0);
  EXPECT_EQ(xCmdIsRunning(exec), 1);

  run_until_done(loop, &ctx, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Busy guard ───────────────────── */

TEST(Cmd, RunWhileBusy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Trying to run again while busy should fail */
  err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  EXPECT_EQ(err, xErrno_Busy);

  run_until_done(loop, &ctx, 5000);
  EXPECT_EQ(ctx.done, 1);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Working directory ───────────────────── */

TEST(Cmd, WorkingDirectory) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  xCmdConf conf = {};
  conf.cmd          = "/bin/pwd";
  conf.cwd          = "/tmp";
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  /* On macOS /tmp is a symlink to /private/tmp */
  EXPECT_TRUE(strcmp(ctx.result.stdout_buf, "/tmp\n") == 0 ||
              strcmp(ctx.result.stdout_buf, "/private/tmp\n") == 0);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Sequential runs ───────────────────── */

TEST(Cmd, SequentialRuns) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Run 1 */
  struct TestCtx ctx1 = {};
  ctx1.loop = loop;

  const char *argv1[] = {"first", nullptr};
  xCmdConf conf1 = {};
  conf1.cmd          = "/bin/echo";
  conf1.argv         = argv1;
  conf1.stdout_mode  = xCmdOutput_Capture;
  conf1.stderr_mode  = xCmdOutput_Discard;

  xErrno err = xCmdRun(exec, &conf1, NULL, NULL, on_done, &ctx1);
  ASSERT_EQ(err, xErrno_Ok);
  run_until_done(loop, &ctx1);
  EXPECT_STREQ(ctx1.result.stdout_buf, "first\n");

  /* Run 2 — reuse the same executor */
  struct TestCtx ctx2 = {};
  ctx2.loop = loop;

  const char *argv2[] = {"second", nullptr};
  xCmdConf conf2 = {};
  conf2.cmd          = "/bin/echo";
  conf2.argv         = argv2;
  conf2.stdout_mode  = xCmdOutput_Capture;
  conf2.stderr_mode  = xCmdOutput_Discard;

  err = xCmdRun(exec, &conf2, NULL, NULL, on_done, &ctx2);
  ASSERT_EQ(err, xErrno_Ok);
  run_until_done(loop, &ctx2);
  EXPECT_STREQ(ctx2.result.stdout_buf, "second\n");

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Null safety ───────────────────── */

TEST(Cmd, NullArgs) {
  EXPECT_EQ(xCmdCreate(NULL), nullptr);
  xCmdDestroy(NULL); /* should not crash */
  EXPECT_EQ(xCmdRun(NULL, NULL, NULL, NULL, NULL, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCmdCancel(NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCmdPid(NULL), -1);
  EXPECT_EQ(xCmdIsRunning(NULL), 0);
  EXPECT_EQ(xCmdPtyFd(NULL), -1);
}

/* ───────────────────── PTY mode ───────────────────── */

TEST(Cmd, PtyCaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", "world", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Discard; /* ignored in PTY mode */
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
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

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyStreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"streaming", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Stream;
  conf.stderr_mode  = xCmdOutput_Discard; /* ignored in PTY mode */
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  /* No separate stdout_buf in Stream mode */
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyMergesStderr) {
  /* In PTY mode, stderr is merged into stdout through the PTY */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Capture; /* ignored in PTY mode */
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
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

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyFdQuery) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running — should return -1 */
  EXPECT_EQ(xCmdPtyFd(exec), -1);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"1", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Capture;
  conf.stderr_mode  = xCmdOutput_Discard;
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int pty_fd = xCmdPtyFd(exec);
  EXPECT_GE(pty_fd, 0);

  run_until_done(loop, &ctx, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyNonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"-c", "exit 42", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.timeout_ms   = 200;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;
  conf.input_mode   = xCmdInput_Pty;

  uint64_t start = xMonoMs();
  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"60", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/sleep";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCmdCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Cmd, PtyDiscardMode) {
  /* PTY with Discard mode: child gets a terminal but we don't read output */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCmd exec = xCmdCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop = loop;

  const char *argv[] = {"hello", nullptr};
  xCmdConf conf = {};
  conf.cmd          = "/bin/echo";
  conf.argv         = argv;
  conf.stdout_mode  = xCmdOutput_Discard;
  conf.stderr_mode  = xCmdOutput_Discard;
  conf.input_mode   = xCmdInput_Pty;

  xErrno err = xCmdRun(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until_done(loop, &ctx);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCmdDestroy(exec);
  xEventLoopDestroy(loop);
}
