/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * command_test.cpp - Tests for xCommandExecutor async command executor
 */

#include <gtest/gtest.h>

#include <x/base/command.h>
#include <x/base/event.h>

#include <string.h>

/* ───────────────────── Helpers ───────────────────── */

struct TestCtx {
  xEventLoop     loop;
  xCommandResult result;
  int            done;
  int            stdout_chunks;
  size_t         total_stdout;
};

static void on_done(xCommandExecutor, const xCommandResult *result, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->result         = *result;
  ctx->done           = 1;
  xEventLoopStop(ctx->loop);
}

static void on_stdout_stream(xCommandExecutor, const char *, size_t len, void *ud) {
  struct TestCtx *ctx = (struct TestCtx *)ud;
  ctx->stdout_chunks++;
  ctx->total_stdout += len;
}

/* ───────────────────── Platform helpers ───────────────────── */

#ifdef _WIN32
/* On Windows we route through cmd.exe /C to get shell builtins. */
static const char *shell_cmd() {
  return "cmd.exe";
}
/* argv for echo: /C echo ... */
/* argv for exit N: /C exit N */
/* argv for sleep: timeout /T N /NOBREAK */
#else
static const char *echo_cmd() {
  return "/bin/echo";
}
static const char *shell_cmd() {
  return "/bin/sh";
}
static const char *sleep_cmd() {
  return "/bin/sleep";
}
#endif

/* ───────────────────── Capture mode ───────────────────── */

TEST(Command, CaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "echo", "hello", "world", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"hello", "world", nullptr};
  conf.cmd           = echo_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Capture;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.signaled, 0);
  EXPECT_EQ(ctx.result.timed_out, 0);
  EXPECT_GT(ctx.result.stdout_len, 0u);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_NE(strstr(ctx.result.stdout_buf, "hello world"), nullptr);
  EXPECT_EQ(ctx.result.stderr_len, 0u);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);
  EXPECT_GT(ctx.result.elapsed_ms, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, CaptureBothStdoutStderr) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "echo out & echo err 1>&2", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Capture;
  conf.stderr_mode = xCommandOutput_Capture;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_NE(strstr(ctx.result.stdout_buf, "out"), nullptr);
  EXPECT_NE(strstr(ctx.result.stderr_buf, "err"), nullptr);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, NonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "exit", "42", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"-c", "exit 42", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, CommandNotFound) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
  conf.cmd          = "/nonexistent/command";
  conf.stdout_mode  = xCommandOutput_Discard;
  conf.stderr_mode  = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
#ifdef _WIN32
  /* On Windows, CreateProcessW fails for non-existent commands,
   * returning xErrno_SysError from Submit rather than exit code 127. */
  EXPECT_EQ(err, xErrno_SysError);
#else
  ASSERT_EQ(err, xErrno_Ok);
  xEventLoopWait(loop, 10000);
  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 127);
#endif

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Stream mode ───────────────────── */

TEST(Command, StreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "echo", "streaming", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"streaming", nullptr};
  conf.cmd           = echo_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Stream;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Discard mode ───────────────────── */

TEST(Command, DiscardAll) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "echo", "discarded", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"discarded", nullptr};
  conf.cmd           = echo_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Timeout ───────────────────── */

TEST(Command, Timeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  /* ping -n 60 127.0.0.1 sleeps ~60s (1 ping/sec). Works without console. */
  const char *argv[] = {"/C", "ping", "-n", "60", "127.0.0.1", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"60", nullptr};
  conf.cmd           = sleep_cmd();
  conf.argv          = argv;
#endif
  conf.timeout_ms  = 200;
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  uint64_t start = xMonoMs();
  xErrno   err   = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Cancel ───────────────────── */

TEST(Command, Cancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  /* ping -n 60 127.0.0.1 sleeps ~60s (1 ping/sec). Works without console. */
  const char *argv[] = {"/C", "ping", "-n", "60", "127.0.0.1", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"60", nullptr};
  conf.cmd           = sleep_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCommandExecutorCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Query ───────────────────── */

TEST(Command, QueryWhileRunning) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running */
  EXPECT_EQ(xCommandExecutorPid(exec), -1);
  EXPECT_EQ(xCommandExecutorIsRunning(exec), 0);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "ping", "-n", "2", "127.0.0.1", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"1", nullptr};
  conf.cmd           = sleep_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running */
  EXPECT_GT(xCommandExecutorPid(exec), 0);
  EXPECT_EQ(xCommandExecutorIsRunning(exec), 1);

  xEventLoopWait(loop, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Busy guard ───────────────────── */

TEST(Command, RunWhileBusy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "ping", "-n", "2", "127.0.0.1", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
#else
  const char *argv[] = {"1", nullptr};
  conf.cmd           = sleep_cmd();
  conf.argv          = argv;
#endif
  conf.stdout_mode = xCommandOutput_Discard;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Trying to run again while busy should fail */
  err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  EXPECT_EQ(err, xErrno_Busy);

  xEventLoopWait(loop, 5000);
  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Working directory ───────────────────── */

TEST(Command, WorkingDirectory) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  xCommandConf conf = {};
#ifdef _WIN32
  const char *argv[] = {"/C", "cd", nullptr};
  conf.cmd           = shell_cmd();
  conf.argv          = argv;
  conf.cwd           = getenv("TEMP"); /* e.g. C:\Users\...\AppData\Local\Temp */
  if (!conf.cwd) conf.cwd = "C:\\";
#else
  const char *argv[] = {nullptr};
  conf.cmd           = "/bin/pwd";
  conf.argv          = argv;
  conf.cwd           = "/tmp";
#endif
  conf.stdout_mode = xCommandOutput_Capture;
  conf.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
#ifdef _WIN32
  /* Just verify we got some output containing a path */
  EXPECT_GT(ctx.result.stdout_len, 0u);
#else
  /* On macOS /tmp is a symlink to /private/tmp */
  EXPECT_TRUE(strcmp(ctx.result.stdout_buf, "/tmp\n") == 0 ||
              strcmp(ctx.result.stdout_buf, "/private/tmp\n") == 0);
#endif

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Sequential runs ───────────────────── */

TEST(Command, SequentialRuns) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Run 1 */
  struct TestCtx ctx1 = {};
  ctx1.loop           = loop;

  xCommandConf conf1 = {};
#ifdef _WIN32
  const char *argv1[] = {"/C", "echo", "first", nullptr};
  conf1.cmd           = shell_cmd();
  conf1.argv          = argv1;
#else
  const char *argv1[] = {"first", nullptr};
  conf1.cmd           = echo_cmd();
  conf1.argv          = argv1;
#endif
  conf1.stdout_mode = xCommandOutput_Capture;
  conf1.stderr_mode = xCommandOutput_Discard;

  xErrno err = xCommandExecutorSubmit(exec, &conf1, NULL, NULL, on_done, &ctx1);
  ASSERT_EQ(err, xErrno_Ok);
  xEventLoopWait(loop, 10000);
  EXPECT_NE(strstr(ctx1.result.stdout_buf, "first"), nullptr);

  /* Run 2 — reuse the same executor */
  struct TestCtx ctx2 = {};
  ctx2.loop           = loop;

  xCommandConf conf2 = {};
#ifdef _WIN32
  const char *argv2[] = {"/C", "echo", "second", nullptr};
  conf2.cmd           = shell_cmd();
  conf2.argv          = argv2;
#else
  const char *argv2[] = {"second", nullptr};
  conf2.cmd           = echo_cmd();
  conf2.argv          = argv2;
#endif
  conf2.stdout_mode = xCommandOutput_Capture;
  conf2.stderr_mode = xCommandOutput_Discard;

  err = xCommandExecutorSubmit(exec, &conf2, NULL, NULL, on_done, &ctx2);
  ASSERT_EQ(err, xErrno_Ok);
  xEventLoopWait(loop, 10000);
  EXPECT_NE(strstr(ctx2.result.stdout_buf, "second"), nullptr);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Null safety ───────────────────── */

TEST(Command, NullArgs) {
  EXPECT_EQ(xCommandExecutorCreate(NULL), nullptr);
  xCommandExecutorDestroy(NULL); /* should not crash */
  EXPECT_EQ(xCommandExecutorSubmit(NULL, NULL, NULL, NULL, NULL, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCommandExecutorCancel(NULL), xErrno_InvalidArg);
  EXPECT_EQ(xCommandExecutorPid(NULL), -1);
  EXPECT_EQ(xCommandExecutorIsRunning(NULL), 0);
  EXPECT_EQ(xCommandExecutorPtyFd(NULL), -1);
}

/* ───────────────────── PTY mode ───────────────────── */

#ifdef _WIN32

TEST(Command, PtyCaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "echo", "hello", "world", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GT(ctx.result.stdout_len, 0u);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "hello world") != nullptr);
  /* PTY fd should be -1 after completion */
  EXPECT_EQ(ctx.result.pty_fd, -1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyStreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "echo", "streaming", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Stream;
  conf.stderr_mode    = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyMergesStderr) {
  /* In PTY mode, stderr is merged into stdout through ConPTY */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "echo out& echo err 1>&2", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Capture; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "out") != nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "err") != nullptr);
  /* stderr_buf should be NULL in PTY mode */
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_len, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyFdQuery) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running — should return -1 */
  EXPECT_EQ(xCommandExecutorPtyFd(exec), -1);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "ping", "-n", "2", "127.0.0.1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int pty_fd = xCommandExecutorPtyFd(exec);
  EXPECT_GE(pty_fd, 0);

  xEventLoopWait(loop, 10000);
  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyStdinFdMatchesPtyFd) {
  /* In PTY mode, StdinFd should return the same fd as PtyFd */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "ping", "-n", "2", "127.0.0.1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_EQ(xCommandExecutorStdinFd(exec), xCommandExecutorPtyFd(exec));
  EXPECT_GE(xCommandExecutorStdinFd(exec), 0);

  xEventLoopWait(loop, 10000);
  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyNonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "exit", "42", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 42);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "ping", "-n", "60", "127.0.0.1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.timeout_ms     = 200;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  uint64_t start = xMonoMs();
  xErrno   err   = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "ping", "-n", "60", "127.0.0.1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCommandExecutorCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyDiscardMode) {
  /* PTY with Discard mode: child gets a terminal but we don't read output */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "echo", "hello", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

#else

TEST(Command, PtyCaptureStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"hello", "world", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/echo";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.signaled, 0);
  EXPECT_GT(ctx.result.stdout_len, 0u);
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  /* In PTY mode, echo output includes CR before LF */
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "hello world") != nullptr);
  /* PTY fd should be -1 after completion */
  EXPECT_EQ(ctx.result.pty_fd, -1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyStreamStdout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"streaming", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/echo";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Stream;
  conf.stderr_mode    = xCommandOutput_Discard; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, on_stdout_stream, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_GE(ctx.stdout_chunks, 1);
  EXPECT_GT(ctx.total_stdout, 0u);
  /* No separate stdout_buf in Stream mode */
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stdout_len, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyMergesStderr) {
  /* In PTY mode, stderr is merged into stdout through the PTY */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"-c", "echo out; echo err >&2", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sh";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Capture; /* ignored in PTY mode */
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  /* Both stdout and stderr output should be in stdout_buf */
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "out") != nullptr);
  EXPECT_TRUE(strstr(ctx.result.stdout_buf, "err") != nullptr);
  /* stderr_buf should be NULL in PTY mode */
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_len, 0u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyFdQuery) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  /* Before running — should return -1 */
  EXPECT_EQ(xCommandExecutorPtyFd(exec), -1);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sleep";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int pty_fd = xCommandExecutorPtyFd(exec);
  EXPECT_GE(pty_fd, 0);

  xEventLoopWait(loop, 5000);

  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyNonZeroExitCode) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/usr/bin/false";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_NE(ctx.result.exit_code, 0);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"60", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sleep";
  conf.argv           = argv;
  conf.timeout_ms     = 200;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  uint64_t start = xMonoMs();
  xErrno   err   = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);
  uint64_t elapsed = xMonoMs() - start;

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);
  EXPECT_LT(elapsed, 5000u);
  EXPECT_GE(elapsed, 150u);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"60", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sleep";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Cancel immediately */
  err = xCommandExecutorCancel(exec);
  EXPECT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.timed_out, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyDiscardMode) {
  /* PTY with Discard mode: child gets a terminal but we don't read output */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"hello", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/echo";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  EXPECT_EQ(ctx.result.stdout_buf, nullptr);
  EXPECT_EQ(ctx.result.stderr_buf, nullptr);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

#endif /* _WIN32 / PTY tests */

/* ───────────────────── Pipe stdin ───────────────────── */

TEST(Command, PipeStdinFdWhenIdle) {
  /* StdinFd should return -1 when no command is running */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  EXPECT_EQ(xCommandExecutorStdinFd(exec), -1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

#ifdef _WIN32
/* On Windows, stdin pipe tests need Windows-compatible commands.
 * The basic query test works, but the write-stdin test requires
 * a command that reads stdin and echoes it — no direct equivalent
 * of `head -n 1`. Skip these on Windows for now. */
TEST(Command, PipeStdinFdWhileRunning_Windows) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"/C", "ping", "-n", "5", "127.0.0.1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = shell_cmd();
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pipe;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int stdin_fd = xCommandExecutorStdinFd(exec);
  EXPECT_GE(stdin_fd, 0);

  /* Cancel to clean up */
  xCommandExecutorCancel(exec);
  xEventLoopWait(loop, 10000);

  /* After completion — should return -1 again */
  EXPECT_EQ(xCommandExecutorStdinFd(exec), -1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}
#else

TEST(Command, PipeStdinFdWhileRunning) {
  /* In Pipe mode, StdinFd should return a valid fd while running */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"5", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sleep";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Discard;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pipe;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* While running — should return a valid fd */
  int stdin_fd = xCommandExecutorStdinFd(exec);
  EXPECT_GE(stdin_fd, 0);

  xEventLoopWait(loop, 10000);
  EXPECT_EQ(ctx.done, 1);

  /* After completion — should return -1 again */
  EXPECT_EQ(xCommandExecutorStdinFd(exec), -1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PipeWriteStdin) {
  /* Write to child's stdin via StdinFd and verify the child receives it.
   * We use `head -n 1` instead of `cat` so the child exits after
   * reading one line, without needing us to close the stdin pipe. */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"-n", "1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/usr/bin/head";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pipe;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  int stdin_fd = xCommandExecutorStdinFd(exec);
  ASSERT_GE(stdin_fd, 0);

  /* Write data to child's stdin */
  const char *msg     = "hello from stdin\n";
  ssize_t     written = write(stdin_fd, msg, strlen(msg));
  EXPECT_EQ(written, (ssize_t)strlen(msg));

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  /* head should have echoed the first line */
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  if (ctx.result.stdout_buf) {
    EXPECT_NE(strstr(ctx.result.stdout_buf, "hello from stdin"), nullptr);
  }

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PipeStdinIsBlocking) {
  /* Verify that the child process's stdin is blocking after the
   * pipe_cloexec_nonblock() → dup2() path.  A non-blocking stdin
   * causes Python's input() to see EAGAIN → EOFError immediately.
   * We run `python3 -c "print(input())"` with pipe-mode stdin and
   * write the input after a short delay.  If stdin is blocking the
   * child blocks on input() until we write; if non-blocking it
   * exits with EOFError before we get a chance. */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"-c", "print(input())", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/usr/bin/python3";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Capture;
  conf.input_mode     = xCommandInput_Pipe;
  conf.timeout_ms     = 5000;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  int stdin_fd = xCommandExecutorStdinFd(exec);
  ASSERT_GE(stdin_fd, 0);

  /* Write input to child's stdin — because stdin is blocking the
   * child is waiting for us. */
  const char *msg     = "hello blocking\n";
  ssize_t     written = write(stdin_fd, msg, strlen(msg));
  EXPECT_EQ(written, (ssize_t)strlen(msg));

  xEventLoopWait(loop, 10000);

  EXPECT_EQ(ctx.done, 1);
  EXPECT_EQ(ctx.result.exit_code, 0);
  /* Python should have printed "hello blocking" */
  EXPECT_NE(ctx.result.stdout_buf, nullptr);
  if (ctx.result.stdout_buf) {
    EXPECT_NE(strstr(ctx.result.stdout_buf, "hello blocking"), nullptr);
  }
  /* stderr should be empty — no EOFError */
  if (ctx.result.stderr_buf) {
    EXPECT_EQ(strstr(ctx.result.stderr_buf, "EOFError"), nullptr);
  }

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}

TEST(Command, PtyStdinFdMatchesPtyFd) {
  /* In PTY mode, StdinFd should return the same fd as PtyFd */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xCommandExecutor exec = xCommandExecutorCreate(loop);
  ASSERT_NE(exec, nullptr);

  struct TestCtx ctx = {};
  ctx.loop           = loop;

  const char  *argv[] = {"1", nullptr};
  xCommandConf conf   = {};
  conf.cmd            = "/bin/sleep";
  conf.argv           = argv;
  conf.stdout_mode    = xCommandOutput_Capture;
  conf.stderr_mode    = xCommandOutput_Discard;
  conf.input_mode     = xCommandInput_Pty;

  xErrno err = xCommandExecutorSubmit(exec, &conf, NULL, NULL, on_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_EQ(xCommandExecutorStdinFd(exec), xCommandExecutorPtyFd(exec));
  EXPECT_GE(xCommandExecutorStdinFd(exec), 0);

  xEventLoopWait(loop, 5000);
  EXPECT_EQ(ctx.done, 1);

  xCommandExecutorDestroy(exec);
  xEventLoopDestroy(loop);
}
#endif /* _WIN32 / POSIX pipe stdin tests */

TEST(Command, PipeStdinFdNullSafety) {
  EXPECT_EQ(xCommandExecutorStdinFd(nullptr), -1);
}
