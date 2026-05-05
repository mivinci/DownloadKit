/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * command.h - Async command executor over xEventLoop
 *
 * Spawns child processes with stdout/stderr capture, streaming, or
 * discard modes. Uses fork() + execvp() with independent process groups
 * for clean timeout/cancellation via killpg(). Child exit detection is
 * done through SIGCHLD delivered via xEventLoopSignalWatch().
 *
 * ── Output modes ──────────────────────────────────────────────────
 *
 *   xCommandOutput_Capture  — accumulate into xCommandResult buffers
 *   xCommandOutput_Stream   — deliver chunks via on_stdout/on_stderr
 *   xCommandOutput_Discard  — redirect to /dev/null
 *
 * ── Input modes ───────────────────────────────────────────────────
 *
 *   xCommandInput_Pipe  — default: stdin is inherited from parent
 *   xCommandInput_Pty   — allocate a pseudo-terminal for the child;
 *                      stdout/stderr are merged into the PTY master,
 *                      and the child gets a proper terminal. This is
 *                      useful for programs that behave differently
 *                      when connected to a terminal (e.g. colored
 *                      output, interactive prompts).
 *
 * When input_mode is xCommandInput_Pty:
 *   - A PTY master/slave pair is created via forkpty() (or
 *     posix_openpt + grantpt + unlockpt on systems without forkpty).
 *   - The child's stdin, stdout, and stderr are all connected to the
 *     slave side of the PTY.
 *   - The parent reads from the master fd; on_stdout receives the
 *     merged output (stderr_mode is effectively ignored — there is
 *     no separate stderr stream in PTY mode).
 *   - In Capture mode, output goes to result.stdout_buf only.
 *   - The on_stderr callback is never invoked in PTY mode.
 *   - result.pty_fd is set to the master fd while the command is
 *     running, allowing the caller to write to the child's stdin.
 *     It is set to -1 after the command completes.
 */

#ifndef XBASE_COMMAND_H
#define XBASE_COMMAND_H

#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/time.h>

/* ───────────────────── Output mode ───────────────────── */

XDEF_ENUM(xCommandOutputMode){
  xCommandOutput_Capture, /**< Accumulate into xCommandResult buffers      */
  xCommandOutput_Stream,  /**< Deliver via on_stdout / on_stderr       */
  xCommandOutput_Discard, /**< Redirect to /dev/null                   */
};

/* ───────────────────── Input mode ───────────────────── */

XDEF_ENUM(xCommandInputMode){
  xCommandInput_Pipe, /**< Default: inherit parent stdin (no PTY)   */
  xCommandInput_Pty,  /**< Allocate a pseudo-terminal for the child */
};

/* ───────────────────── Configuration ───────────────────── */

XDEF_STRUCT(xCommandConf) {
  const char  *cmd;  /**< Program path (required, searched in $PATH) */
  const char **argv; /**< Argument vector (NULL-terminated, may be NULL) */
  const char **envp; /**< Environment (NULL = inherit parent) */
  const char  *cwd;  /**< Working directory (NULL = inherit)  */

  uint64_t timeout_ms; /**< 0 = no timeout                      */
  size_t   stdout_cap; /**< Max stdout bytes (0 = unlimited)    */
  size_t
    stderr_cap; /**< Max stderr bytes (0 = unlimited, ignored in PTY mode) */

  xCommandOutputMode stdout_mode;
  xCommandOutputMode
    stderr_mode; /**< Ignored in PTY mode (merged into stdout) */

  xCommandInputMode
    input_mode; /**< xCommandInput_Pipe (default) or xCommandInput_Pty */
};

/* ───────────────────── Result ───────────────────── */

XDEF_STRUCT(xCommandResult) {
  int exit_code; /**< Exit status (valid if signaled == 0) */
  int signaled;  /**< Non-zero if killed by signal; holds signal# */
  int timed_out; /**< Non-zero if killed due to timeout    */

  const char *stdout_buf; /**< Captured stdout (NULL in Stream/Discard) */
  size_t      stdout_len;
  const char *stderr_buf; /**< Captured stderr (NULL in Stream/Discard/PTY) */
  size_t      stderr_len;

  uint64_t elapsed_ms; /**< Wall-clock duration from spawn to exit */

  int pty_fd; /**< PTY master fd (valid while running, -1 otherwise) */
};

/* ───────────────────── Handle ───────────────────── */

XDEF_HANDLE(xCommandExecutor);

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Streaming output callback (only in xCommandOutput_Stream mode).
 *
 * @param exec  The executor handle.
 * @param data  Pointer to the output chunk (not NUL-terminated).
 * @param len   Number of bytes in @p data.
 * @param ud    User-provided argument.
 */
typedef void (*xCommandExecutorOutputFunc)(xCommandExecutor exec,
                                           const char *data, size_t len,
                                           void *ud);

/**
 * @brief Completion callback.
 *
 * @param exec   The executor handle.
 * @param result Pointer to the result (valid only inside this callback).
 * @param ud     User-provided argument.
 */
typedef void (*xCommandExecutorDoneFunc)(xCommandExecutor      exec,
                                         const xCommandResult *result,
                                         void                 *ud);

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a command executor bound to the given event loop.
 *
 * Registers a SIGCHLD watch on the event loop for child exit detection.
 *
 * @param loop  Event loop (must not be NULL).
 * @return      A new executor, or NULL on failure.
 */
XCAPI(xCommandExecutor) xCommandExecutorCreate(xEventLoop loop);

/**
 * @brief Destroy a command executor.
 *
 * If a command is running, kills the child process group (SIGKILL)
 * and waits for it before freeing resources.
 *
 * @param exec  Executor handle (NULL is safe).
 */
XCAPI(void) xCommandExecutorDestroy(xCommandExecutor exec);

/* ───────────────────── Execution ───────────────────── */

/**
 * @brief Submit a command for asynchronous execution.
 *
 * Spawns the child process and returns immediately. The result is
 * delivered via the @p on_done callback on the event loop thread.
 *
 * An executor can only run one command at a time. Call this again
 * only after the previous on_done has fired.
 *
 * @param exec      Executor handle (must not be NULL).
 * @param conf      Command configuration (must not be NULL, cmd must be set).
 * @param on_stdout Callback for stdout chunks (NULL if not streaming).
 *                  In PTY mode, this receives all merged output.
 * @param on_stderr Callback for stderr chunks (NULL if not streaming).
 *                  Never called in PTY mode.
 * @param on_done   Completion callback (must not be NULL).
 * @param ud        User data forwarded to all callbacks.
 * @return          xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xCommandExecutorSubmit(xCommandExecutor           exec,
                                     const xCommandConf        *conf,
                                     xCommandExecutorOutputFunc on_stdout,
                                     xCommandExecutorOutputFunc on_stderr,
                                     xCommandExecutorDoneFunc   on_done,
                                     void                      *ud);

/**
 * @brief Cancel a running command.
 *
 * Sends SIGTERM to the child process group. If the child does not
 * exit within a grace period (5 seconds), sends SIGKILL.
 * The on_done callback will still fire with timed_out=1.
 *
 * @param exec  Executor handle (must not be NULL).
 * @return      xErrno_Ok on success, xErrno_InvalidState if not running.
 */
XCAPI(xErrno) xCommandExecutorCancel(xCommandExecutor exec);

/* ───────────────────── Query ───────────────────── */

/**
 * @brief Return the PID of the running child, or -1 if idle.
 *
 * @param exec  Executor handle (NULL-safe).
 * @return      Child PID or -1.
 */
XCAPI(int) xCommandExecutorPid(xCommandExecutor exec);

/**
 * @brief Return whether a command is currently running.
 *
 * @param exec  Executor handle (NULL-safe).
 * @return      Non-zero if running.
 */
XCAPI(int) xCommandExecutorIsRunning(xCommandExecutor exec);

/**
 * @brief Return the PTY master fd, or -1 if not in PTY mode or not running.
 *
 * The caller can write to this fd to send input to the child's stdin.
 * The fd is closed automatically when the command finishes.
 *
 * @param exec  Executor handle (NULL-safe).
 * @return      PTY master fd or -1.
 */
XCAPI(int) xCommandExecutorPtyFd(xCommandExecutor exec);

/**
 * @brief Return a writable fd for the child's stdin, or -1 if not available.
 *
 * In PTY mode this returns the PTY master fd (same as xCommandExecutorPtyFd).
 * In Pipe mode this returns the write end of the stdin pipe. The child's
 * stdin is connected to the read end; writing to this fd sends data to the
 * child's standard input.
 *
 * The fd is closed automatically when the command finishes or the executor
 * is destroyed. The caller should NOT close the returned fd.
 *
 * @param exec  Executor handle (NULL-safe).
 * @return      Writable fd for child stdin, or -1 if not running / not available.
 */
XCAPI(int) xCommandExecutorStdinFd(xCommandExecutor exec);

#endif /* XBASE_COMMAND_H */
