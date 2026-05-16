/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * command_windows.c - Async command executor for Windows
 *
 * Uses CreateProcessW() for process creation, CreatePipe() for I/O
 * redirection, and RegisterWaitForSingleObject() for child exit
 * detection. Pipe reading is done on dedicated threads (since WSAPoll
 * cannot poll anonymous pipe HANDLEs), with chunks posted back to the
 * event loop via xEventLoopPost().
 *
 * PTY mode (xCommandInput_Pty) uses the Windows ConPTY API
 * (CreatePseudoConsole) introduced in Windows 10 1809.  The ConPTY
 * input/output pipes are read/written on the same reader threads as
 * the regular pipe mode, and the parent-side write handle is exposed
 * via PtyFd / StdinFd (converted to a CRT fd via _open_osfhandle).
 */

#ifdef _WIN32

#include "event_private.h"
#include <x/base/command.h>
#include <x/base/string.h>

#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ConPTY API — available on Windows 10 1809+ (build 17763).
 * We resolve at runtime so the binary still loads on older Windows. */
typedef HRESULT(WINAPI *pfn_CreatePseudoConsole)(COORD size, HANDLE hInput,
                                                 HANDLE hOutput, DWORD dwFlags,
                                                 HPCON *phPC);
typedef void(WINAPI *pfn_ClosePseudoConsole)(HPCON hPC);

/* These may not be defined in older SDKs */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
  ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

static pfn_CreatePseudoConsole fn_CreatePseudoConsole = NULL;
static pfn_ClosePseudoConsole  fn_ClosePseudoConsole  = NULL;
static int                     conpty_resolved        = 0;

static int conpty_resolve(void) {
  if (conpty_resolved) return (fn_CreatePseudoConsole != NULL);
  conpty_resolved = 1;
  HMODULE h       = GetModuleHandleW(L"kernel32.dll");
  if (!h) return 0;
  fn_CreatePseudoConsole =
    (pfn_CreatePseudoConsole)GetProcAddress(h, "CreatePseudoConsole");
  fn_ClosePseudoConsole =
    (pfn_ClosePseudoConsole)GetProcAddress(h, "ClosePseudoConsole");
  return (fn_CreatePseudoConsole != NULL);
}

/* ───────────────────── Constants ───────────────────── */

#define CMD_CANCEL_GRACE_MS 5000 /**< TerminateProcess retry interval    */
#define CMD_READ_BUF_SIZE   4096 /**< Per-read buffer size                */

/* ───────────────────── Internal state ───────────────────── */

enum xCommandExecutorState_ {
  xCommandExecutorState_Idle = 0,
  xCommandExecutorState_Running,
  xCommandExecutorState_Cancelling,
};

struct xCommandExecutor_ {
  xEventLoop loop;

  /* Child process */
  HANDLE                      hProcess;
  HANDLE                      hPrimaryThread; /* from CreateProcess */
  DWORD                       child_pid;
  enum xCommandExecutorState_ state;
  int                         destroying;   /* set during destroy  */
  int                         force_killed; /* TerminateProcess'd  */

  /* Stdin pipe: parent writes to hStdinWrite, child reads from it */
  HANDLE hStdinWrite;
  int    stdin_fd; /* CRT fd from _open_osfhandle, -1 if N/A */

  /* Stdout/stderr pipe read ends (parent side) */
  HANDLE hStdoutRead;
  HANDLE hStderrRead;

  /* Process exit wait registration */
  HANDLE hWait;

  /* ConPTY state (valid only in PTY mode) */
  HPCON  hConPty;        /* pseudo-console handle            */
  HANDLE hPtyConInWrite; /* parent writes to ConPTY input    */
  HANDLE hPtyConOutRead; /* parent reads from ConPTY output  */
  int    pty_fd;         /* CRT fd for hPtyConInWrite (-1=N/A) */

  /* Pipe reader threads */
  HANDLE hStdoutThread;
  HANDLE hStderrThread;

  /* Timeout / cancel-grace timers */
  xEventTimer timeout_timer;
  xEventTimer cancel_timer;

  /* Capture buffers */
  xString stdout_buf;
  xString stderr_buf;
  size_t  stdout_max;
  size_t  stderr_max;

  /* Output / input modes (saved from xCommandConf) */
  xCommandOutputMode stdout_mode;
  xCommandOutputMode stderr_mode;
  xCommandInputMode  input_mode;

  /* Result */
  xCommandResult result;

  /* Callbacks */
  xCommandExecutorOutputFunc on_stdout;
  xCommandExecutorOutputFunc on_stderr;
  xCommandExecutorDoneFunc   on_done;
  void                      *ud;

  /* Timing */
  uint64_t start_ms;

  /* Completion tracking */
  int stdout_eof;
  int stderr_eof;
  int child_exited;
};

/* ───────────────────── Pipe chunk (posted from reader → event loop) ─── */

struct PipeChunk_ {
  struct xCommandExecutor_ *exec;
  char                     *data; /* heap-allocated (NULL for EOF) */
  size_t                    len;
  int                       is_stderr;
  int                       eof;
};

/* ───────────────────── Process exit info (posted from TP → event loop) */

struct ProcessExitInfo_ {
  struct xCommandExecutor_ *exec;
  DWORD                     exit_code;
};

/* ───────────────────── Pipe reader context ───────────────────── */

struct PipeReaderCtx_ {
  HANDLE                    hRead;
  struct xCommandExecutor_ *exec;
  int                       is_stderr;
};

/* ───────────────────── Forward declarations ───────────────────── */

static void          cmd_check_completion(struct xCommandExecutor_ *exec);
static void          cmd_fire_done(struct xCommandExecutor_ *exec);
static void          on_timeout(void *arg);
static void          on_cancel_grace(void *arg);
static void          on_pipe_chunk(void *arg);
static void          on_process_exit_posted(void *arg);
static void CALLBACK on_process_exit(PVOID param, BOOLEAN fired);
static DWORD WINAPI  pipe_reader_thread(LPVOID param);
static wchar_t      *build_command_line(const char *cmd, const char **argv);
static xErrno        xCommandExecutorSubmitPty(struct xCommandExecutor_ *exec,
                                               const xCommandConf       *conf);

/* ═══════════════════════════════════════════════════════════════════
 * Pipe chunk callback — runs on the event loop thread
 * ═══════════════════════════════════════════════════════════════════ */

static void on_pipe_chunk(void *arg) {
  struct PipeChunk_        *chunk = (struct PipeChunk_ *)arg;
  struct xCommandExecutor_ *exec  = chunk->exec;

  if (exec->destroying) {
    free(chunk->data);
    free(chunk);
    return;
  }

  if (chunk->eof) {
    if (chunk->is_stderr)
      exec->stderr_eof = 1;
    else
      exec->stdout_eof = 1;
    free(chunk);
    cmd_check_completion(exec);
    return;
  }

  /* Data chunk */
  if (chunk->is_stderr) {
    if (exec->stderr_mode == xCommandOutput_Capture) {
      size_t cap = exec->stderr_max;
      if (cap == 0 || xStringLen(exec->stderr_buf) + chunk->len <= cap)
        xStringAppendLen(&exec->stderr_buf, chunk->data, chunk->len);
    } else if (exec->stderr_mode == xCommandOutput_Stream && exec->on_stderr) {
      exec->on_stderr((xCommandExecutor)exec, chunk->data, chunk->len,
                      exec->ud);
    }
  } else {
    if (exec->stdout_mode == xCommandOutput_Capture) {
      size_t cap = exec->stdout_max;
      if (cap == 0 || xStringLen(exec->stdout_buf) + chunk->len <= cap)
        xStringAppendLen(&exec->stdout_buf, chunk->data, chunk->len);
    } else if (exec->stdout_mode == xCommandOutput_Stream && exec->on_stdout) {
      exec->on_stdout((xCommandExecutor)exec, chunk->data, chunk->len,
                      exec->ud);
    }
  }

  free(chunk->data);
  free(chunk);
}

/* ═══════════════════════════════════════════════════════════════════
 * Pipe reader thread — reads from a pipe and posts chunks
 * ═══════════════════════════════════════════════════════════════════ */

static DWORD WINAPI pipe_reader_thread(LPVOID param) {
  struct PipeReaderCtx_ *ctx = (struct PipeReaderCtx_ *)param;
  char                   buf[CMD_READ_BUF_SIZE];

  for (;;) {
    DWORD bytesRead = 0;
    BOOL  ok        = ReadFile(ctx->hRead, buf, sizeof(buf), &bytesRead, NULL);
    if (!ok || bytesRead == 0) break; /* EOF or error */

    struct PipeChunk_ *chunk = (struct PipeChunk_ *)malloc(sizeof(*chunk));
    if (!chunk) break;
    chunk->exec      = ctx->exec;
    chunk->is_stderr = ctx->is_stderr;
    chunk->eof       = 0;
    chunk->len       = bytesRead;
    chunk->data      = (char *)malloc(bytesRead);
    if (!chunk->data) {
      free(chunk);
      break;
    }
    memcpy(chunk->data, buf, bytesRead);
    xEventLoopPost(ctx->exec->loop, on_pipe_chunk, chunk);
  }

  /* Post EOF marker */
  struct PipeChunk_ *eof_chunk =
    (struct PipeChunk_ *)calloc(1, sizeof(*eof_chunk));
  if (eof_chunk) {
    eof_chunk->exec      = ctx->exec;
    eof_chunk->is_stderr = ctx->is_stderr;
    eof_chunk->eof       = 1;
    xEventLoopPost(ctx->exec->loop, on_pipe_chunk, eof_chunk);
  }

  free(ctx);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Process exit — TP callback → event loop post
 * ═══════════════════════════════════════════════════════════════════ */

static void CALLBACK on_process_exit(PVOID param, BOOLEAN fired) {
  (void)fired;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)param;
  if (exec->destroying) return;

  DWORD exitCode = 0;
  GetExitCodeProcess(exec->hProcess, &exitCode);

  struct ProcessExitInfo_ *info =
    (struct ProcessExitInfo_ *)malloc(sizeof(*info));
  if (!info) return;
  info->exec      = exec;
  info->exit_code = exitCode;
  xEventLoopPost(exec->loop, on_process_exit_posted, info);
}

static void on_process_exit_posted(void *arg) {
  struct ProcessExitInfo_  *info = (struct ProcessExitInfo_ *)arg;
  struct xCommandExecutor_ *exec = info->exec;

  if (exec->destroying) {
    free(info);
    return;
  }

  if (!exec->child_exited) {
    exec->child_exited = 1;
    if (exec->force_killed)
      exec->result.signaled = 1;
    else
      exec->result.signaled = 0;
    exec->result.exit_code = (int)info->exit_code;
    cmd_check_completion(exec);
  }

  free(info);
}

/* ═══════════════════════════════════════════════════════════════════
 * Completion check / fire done
 * ═══════════════════════════════════════════════════════════════════ */

static void cmd_check_completion(struct xCommandExecutor_ *exec) {
  /* In PTY mode, close ConPTY as soon as the child exits so that the
   * output reader thread unblocks (ConPTY may keep the output pipe open
   * until ClosePseudoConsole is called, causing a deadlock: we wait for
   * stdout_eof which never arrives because the pipe never closes). */
  if (exec->child_exited && exec->input_mode == xCommandInput_Pty &&
      exec->hConPty) {
    fn_ClosePseudoConsole(exec->hConPty);
    exec->hConPty = NULL;
  }

  if (!exec->stdout_eof || !exec->stderr_eof || !exec->child_exited) return;
  cmd_fire_done(exec);
}

static void cmd_fire_done(struct xCommandExecutor_ *exec) {
  /* Cancel timers */
  if (exec->timeout_timer) {
    xEventLoopTimerCancel(exec->loop, exec->timeout_timer);
    exec->timeout_timer = NULL;
  }
  if (exec->cancel_timer) {
    xEventLoopTimerCancel(exec->loop, exec->cancel_timer);
    exec->cancel_timer = NULL;
  }

  /* Unregister process wait */
  if (exec->hWait) {
    UnregisterWait(exec->hWait);
    exec->hWait = NULL;
  }

  /* Finalize result */
  exec->result.elapsed_ms = xMonoMs() - exec->start_ms;

  if (exec->stdout_mode == xCommandOutput_Capture && exec->stdout_buf) {
    exec->result.stdout_buf = exec->stdout_buf;
    exec->result.stdout_len = xStringLen(exec->stdout_buf);
  }
  if (exec->stderr_mode == xCommandOutput_Capture &&
      exec->input_mode != xCommandInput_Pty && exec->stderr_buf) {
    exec->result.stderr_buf = exec->stderr_buf;
    exec->result.stderr_len = xStringLen(exec->stderr_buf);
  }
  exec->result.pty_fd = -1;

  /* Wait for reader threads (they should be done — pipes are closed) */
  if (exec->hStdoutThread) {
    WaitForSingleObject(exec->hStdoutThread, 5000);
    CloseHandle(exec->hStdoutThread);
    exec->hStdoutThread = NULL;
  }
  if (exec->hStderrThread) {
    WaitForSingleObject(exec->hStderrThread, 5000);
    CloseHandle(exec->hStderrThread);
    exec->hStderrThread = NULL;
  }

  /* Close pipe handles */
  if (exec->hStdoutRead) {
    CloseHandle(exec->hStdoutRead);
    exec->hStdoutRead = NULL;
  }
  if (exec->hStderrRead) {
    CloseHandle(exec->hStderrRead);
    exec->hStderrRead = NULL;
  }
  if (exec->hStdinWrite) {
    CloseHandle(exec->hStdinWrite);
    exec->hStdinWrite = NULL;
  }
  if (exec->stdin_fd >= 0) {
    _close(exec->stdin_fd);
    exec->stdin_fd = -1;
  }

  /* Close ConPTY handles */
  if (exec->hPtyConOutRead) {
    CloseHandle(exec->hPtyConOutRead);
    exec->hPtyConOutRead = NULL;
  }
  if (exec->hPtyConInWrite) {
    CloseHandle(exec->hPtyConInWrite);
    exec->hPtyConInWrite = NULL;
  }
  if (exec->pty_fd >= 0) {
    _close(exec->pty_fd);
    exec->pty_fd = -1;
  }
  if (exec->hConPty) {
    fn_ClosePseudoConsole(exec->hConPty);
    exec->hConPty = NULL;
  }

  /* Close process handles */
  if (exec->hProcess) {
    CloseHandle(exec->hProcess);
    exec->hProcess = NULL;
  }
  if (exec->hPrimaryThread) {
    CloseHandle(exec->hPrimaryThread);
    exec->hPrimaryThread = NULL;
  }

  /* Transition to idle BEFORE callback (so Submit can be called again) */
  exec->state = xCommandExecutorState_Idle;

  /* Deliver result — copy because callback may start a new run */
  xCommandResult result_copy = exec->result;
  exec->on_done((xCommandExecutor)exec, &result_copy, exec->ud);
}

/* ═══════════════════════════════════════════════════════════════════
 * Timeout callback
 * ═══════════════════════════════════════════════════════════════════ */

static void on_timeout(void *arg) {
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  exec->timeout_timer            = NULL;

  if (exec->state != xCommandExecutorState_Running) return;

  exec->force_killed     = 1;
  exec->result.timed_out = 1;
  exec->state            = xCommandExecutorState_Cancelling;

  if (exec->hProcess) TerminateProcess(exec->hProcess, 1);

  /* Grace timer: retry TerminateProcess if the first one didn't work */
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                            CMD_CANCEL_GRACE_MS);
}

/* ═══════════════════════════════════════════════════════════════════
 * Cancel grace period
 * ═══════════════════════════════════════════════════════════════════ */

static void on_cancel_grace(void *arg) {
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  exec->cancel_timer             = NULL;
  if (exec->state != xCommandExecutorState_Cancelling) return;
  if (exec->hProcess) TerminateProcess(exec->hProcess, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * Build command line from cmd + argv
 * ═══════════════════════════════════════════════════════════════════ */

static wchar_t *build_command_line(const char *cmd, const char **argv) {
  /* Estimate length */
  size_t len = strlen(cmd) * 2 + 4; /* quotes + space + NUL */
  if (argv) {
    for (int i = 0; argv[i]; i++)
      len += strlen(argv[i]) * 2 + 4;
  }

  char *buf = (char *)malloc(len);
  if (!buf) return NULL;
  buf[0] = '\0';

  /* Append cmd (quote if it contains spaces) */
  if (strchr(cmd, ' ') || strchr(cmd, '\t'))
    snprintf(buf, len, "\"%s\"", cmd);
  else
    snprintf(buf, len, "%s", cmd);

  /* Append argv */
  if (argv) {
    for (int i = 0; argv[i]; i++) {
      strcat(buf, " ");
      if (strchr(argv[i], ' ') || strchr(argv[i], '\t')) {
        size_t cur = strlen(buf);
        snprintf(buf + cur, len - cur, "\"%s\"", argv[i]);
      } else {
        strcat(buf, argv[i]);
      }
    }
  }

  /* Convert UTF-8 → UTF-16 */
  int      wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
  wchar_t *wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
  if (!wbuf) {
    free(buf);
    return NULL;
  }
  MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wlen);
  free(buf);
  return wbuf;
}

/* ═══════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

xCommandExecutor xCommandExecutorCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xCommandExecutor_ *exec =
    (struct xCommandExecutor_ *)calloc(1, sizeof(*exec));
  if (!exec) return NULL;

  exec->loop           = loop;
  exec->hProcess       = NULL;
  exec->hPrimaryThread = NULL;
  exec->hStdinWrite    = NULL;
  exec->hStdoutRead    = NULL;
  exec->hStderrRead    = NULL;
  exec->hWait          = NULL;
  exec->hConPty        = NULL;
  exec->hPtyConInWrite = NULL;
  exec->hPtyConOutRead = NULL;
  exec->pty_fd         = -1;
  exec->hStdoutThread  = NULL;
  exec->hStderrThread  = NULL;
  exec->stdin_fd       = -1;
  exec->timeout_timer  = NULL;
  exec->cancel_timer   = NULL;
  exec->stdout_buf     = NULL;
  exec->stderr_buf     = NULL;
  exec->state          = xCommandExecutorState_Idle;
  exec->destroying     = 0;
  exec->force_killed   = 0;

  return (xCommandExecutor)exec;
}

void xCommandExecutorDestroy(xCommandExecutor exec_) {
  if (!exec_) return;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;

  if (exec->state != xCommandExecutorState_Idle) {
    exec->destroying = 1;

    /* Kill the child process */
    if (exec->hProcess) {
      TerminateProcess(exec->hProcess, 1);
      WaitForSingleObject(exec->hProcess, 5000);
    }

    /* Unregister process wait (block until pending callback finishes) */
    if (exec->hWait) {
      UnregisterWaitEx(exec->hWait, INVALID_HANDLE_VALUE);
      exec->hWait = NULL;
    }

    /* Close pipe read handles to unblock reader threads */
    if (exec->hStdoutRead) {
      CloseHandle(exec->hStdoutRead);
      exec->hStdoutRead = NULL;
    }
    if (exec->hStderrRead) {
      CloseHandle(exec->hStderrRead);
      exec->hStderrRead = NULL;
    }
    if (exec->hPtyConOutRead) {
      CloseHandle(exec->hPtyConOutRead);
      exec->hPtyConOutRead = NULL;
    }

    /* Wait for reader threads to finish */
    if (exec->hStdoutThread) {
      WaitForSingleObject(exec->hStdoutThread, 5000);
      CloseHandle(exec->hStdoutThread);
      exec->hStdoutThread = NULL;
    }
    if (exec->hStderrThread) {
      WaitForSingleObject(exec->hStderrThread, 5000);
      CloseHandle(exec->hStderrThread);
      exec->hStderrThread = NULL;
    }

    /* Drain any pending posted chunks (they'll see destroying=1) */
    xEventWait(exec->loop, 0);

    /* Cancel timers */
    if (exec->timeout_timer) {
      xEventLoopTimerCancel(exec->loop, exec->timeout_timer);
      exec->timeout_timer = NULL;
    }
    if (exec->cancel_timer) {
      xEventLoopTimerCancel(exec->loop, exec->cancel_timer);
      exec->cancel_timer = NULL;
    }

    /* Close remaining handles */
    if (exec->hStdinWrite) {
      CloseHandle(exec->hStdinWrite);
      exec->hStdinWrite = NULL;
    }
    if (exec->stdin_fd >= 0) {
      _close(exec->stdin_fd);
      exec->stdin_fd = -1;
    }
    if (exec->hPtyConInWrite) {
      CloseHandle(exec->hPtyConInWrite);
      exec->hPtyConInWrite = NULL;
    }
    if (exec->pty_fd >= 0) {
      _close(exec->pty_fd);
      exec->pty_fd = -1;
    }
    if (exec->hConPty && fn_ClosePseudoConsole) {
      fn_ClosePseudoConsole(exec->hConPty);
      exec->hConPty = NULL;
    }
    if (exec->hProcess) {
      CloseHandle(exec->hProcess);
      exec->hProcess = NULL;
    }
    if (exec->hPrimaryThread) {
      CloseHandle(exec->hPrimaryThread);
      exec->hPrimaryThread = NULL;
    }
  }

  if (exec->stdout_buf) xStringDestroy(exec->stdout_buf);
  if (exec->stderr_buf) xStringDestroy(exec->stderr_buf);
  free(exec);
}

/* ═══════════════════════════════════════════════════════════════════
 * Execution
 * ═══════════════════════════════════════════════════════════════════ */

xErrno xCommandExecutorSubmit(xCommandExecutor exec_, const xCommandConf *conf,
                              xCommandExecutorOutputFunc on_stdout,
                              xCommandExecutorOutputFunc on_stderr,
                              xCommandExecutorDoneFunc on_done, void *ud) {
  if (!exec_ || !conf || !conf->cmd || !on_done) return xErrno_InvalidArg;

  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;

  if (exec->state != xCommandExecutorState_Idle) return xErrno_Busy;

  /* ── Reset state from previous run ── */
  if (exec->stdout_buf) xStringDestroy(exec->stdout_buf);
  if (exec->stderr_buf) xStringDestroy(exec->stderr_buf);
  memset(&exec->result, 0, sizeof(exec->result));
  exec->stdout_buf     = NULL;
  exec->stderr_buf     = NULL;
  exec->stdout_max     = conf->stdout_cap;
  exec->stderr_max     = conf->stderr_cap;
  exec->stdout_mode    = conf->stdout_mode;
  exec->stderr_mode    = conf->stderr_mode;
  exec->input_mode     = conf->input_mode;
  exec->destroying     = 0;
  exec->force_killed   = 0;
  exec->stdout_eof     = 0;
  exec->stderr_eof     = 0;
  exec->child_exited   = 0;
  exec->result.pty_fd  = -1;
  exec->stdin_fd       = -1;
  exec->hStdinWrite    = NULL;
  exec->hStdoutRead    = NULL;
  exec->hStderrRead    = NULL;
  exec->hWait          = NULL;
  exec->hConPty        = NULL;
  exec->hPtyConInWrite = NULL;
  exec->hPtyConOutRead = NULL;
  exec->pty_fd         = -1;
  exec->hStdoutThread  = NULL;
  exec->hStderrThread  = NULL;
  exec->hProcess       = NULL;
  exec->hPrimaryThread = NULL;
  exec->timeout_timer  = NULL;
  exec->cancel_timer   = NULL;

  if (conf->stdout_mode == xCommandOutput_Capture) {
    exec->stdout_buf = xStringCreate(NULL);
    if (!exec->stdout_buf) goto fail;
  }
  if (conf->stderr_mode == xCommandOutput_Capture) {
    exec->stderr_buf = xStringCreate(NULL);
    if (!exec->stderr_buf) goto fail;
  }

  exec->on_stdout = on_stdout;
  exec->on_stderr = on_stderr;
  exec->on_done   = on_done;
  exec->ud        = ud;

  /* ── PTY mode: delegate to ConPTY implementation ── */
  if (conf->input_mode == xCommandInput_Pty)
    return xCommandExecutorSubmitPty(exec, conf);

  /* ── Create pipes ── */
  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

  HANDLE hStdinRead   = NULL; /* child's stdin read end  */
  HANDLE hStdoutWrite = NULL; /* child's stdout write end */
  HANDLE hStderrWrite = NULL; /* child's stderr write end */
  HANDLE hNulStdout   = NULL; /* NUL for Discard mode     */
  HANDLE hNulStderr   = NULL;

  /* Stdin pipe: parent writes → child reads */
  if (!CreatePipe(&hStdinRead, &exec->hStdinWrite, &sa, 0)) goto fail;
  SetHandleInformation(exec->hStdinWrite, HANDLE_FLAG_INHERIT, 0);

  /* Stdout pipe */
  if (conf->stdout_mode != xCommandOutput_Discard) {
    if (!CreatePipe(&exec->hStdoutRead, &hStdoutWrite, &sa, 0)) goto fail;
    SetHandleInformation(exec->hStdoutRead, HANDLE_FLAG_INHERIT, 0);
  } else {
    hNulStdout =
      CreateFileW(L"NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (hNulStdout == INVALID_HANDLE_VALUE) goto fail;
  }

  /* Stderr pipe */
  if (conf->stderr_mode != xCommandOutput_Discard) {
    if (!CreatePipe(&exec->hStderrRead, &hStderrWrite, &sa, 0)) goto fail;
    SetHandleInformation(exec->hStderrRead, HANDLE_FLAG_INHERIT, 0);
  } else {
    hNulStderr =
      CreateFileW(L"NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (hNulStderr == INVALID_HANDLE_VALUE) goto fail;
  }

  /* ── Build command line ── */
  wchar_t *wcmdline = build_command_line(conf->cmd, conf->argv);
  if (!wcmdline) goto fail;

  /* ── Set up startup info ── */
  STARTUPINFOW si;
  memset(&si, 0, sizeof(si));
  si.cb        = sizeof(si);
  si.dwFlags   = STARTF_USESTDHANDLES;
  si.hStdInput = hStdinRead;
  si.hStdOutput =
    (conf->stdout_mode != xCommandOutput_Discard) ? hStdoutWrite : hNulStdout;
  si.hStdError =
    (conf->stderr_mode != xCommandOutput_Discard) ? hStderrWrite : hNulStderr;

  /* ── Working directory ── */
  wchar_t *wcwd = NULL;
  if (conf->cwd) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, conf->cwd, -1, NULL, 0);
    wcwd     = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (wcwd) MultiByteToWideChar(CP_UTF8, 0, conf->cwd, -1, wcwd, wlen);
  }

  /* ── Create process ── */
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));

  exec->start_ms = xMonoMs();

  BOOL ok =
    CreateProcessW(NULL,     /* lpApplicationName — search PATH via cmdline */
                   wcmdline, /* lpCommandLine     */
                   NULL,     /* lpProcessAttributes */
                   NULL,     /* lpThreadAttributes  */
                   TRUE,     /* bInheritHandles     */
                   CREATE_NEW_PROCESS_GROUP, /* dwCreationFlags */
                   NULL, /* lpEnvironment — inherit parent's */
                   wcwd, /* lpCurrentDirectory */
                   &si,  /* lpStartupInfo */
                   &pi   /* lpProcessInformation */
    );

  free(wcmdline);
  free(wcwd);

  /* Close NUL handles (child inherited them; parent doesn't need them) */
  if (hNulStdout) CloseHandle(hNulStdout);
  if (hNulStderr) CloseHandle(hNulStderr);

  if (!ok) goto fail;

  exec->hProcess       = pi.hProcess;
  exec->hPrimaryThread = pi.hThread;
  exec->child_pid      = pi.dwProcessId;

  /* Close child-side handles (child owns them now) */
  CloseHandle(hStdinRead);
  hStdinRead = NULL;
  if (hStdoutWrite) {
    CloseHandle(hStdoutWrite);
    hStdoutWrite = NULL;
  }
  if (hStderrWrite) {
    CloseHandle(hStderrWrite);
    hStderrWrite = NULL;
  }

  /* Convert stdin write HANDLE to CRT fd */
  exec->stdin_fd =
    _open_osfhandle((intptr_t)exec->hStdinWrite, _O_WRONLY | _O_BINARY);
  /* After _open_osfhandle, CRT owns the HANDLE.
   * Close it via _close(stdin_fd), NOT CloseHandle. */
  if (exec->stdin_fd >= 0) exec->hStdinWrite = NULL; /* CRT owns it now */

  /* ── Register process exit wait ── */
  if (!RegisterWaitForSingleObject(&exec->hWait, exec->hProcess,
                                   on_process_exit, exec, INFINITE,
                                   WT_EXECUTEONLYONCE)) {
    exec->hWait = NULL; /* proceed without wait — probe below may catch it */
  }

  /* ── Start pipe reader threads ── */
  if (exec->hStdoutRead) {
    struct PipeReaderCtx_ *ctx = (struct PipeReaderCtx_ *)malloc(sizeof(*ctx));
    if (!ctx) goto fail_process;
    ctx->hRead     = exec->hStdoutRead;
    ctx->exec      = exec;
    ctx->is_stderr = 0;
    exec->hStdoutThread =
      CreateThread(NULL, 0, pipe_reader_thread, ctx, 0, NULL);
    if (!exec->hStdoutThread) {
      free(ctx);
      goto fail_process;
    }
  } else {
    exec->stdout_eof = 1;
  }

  if (exec->hStderrRead) {
    struct PipeReaderCtx_ *ctx = (struct PipeReaderCtx_ *)malloc(sizeof(*ctx));
    if (!ctx) goto fail_process;
    ctx->hRead     = exec->hStderrRead;
    ctx->exec      = exec;
    ctx->is_stderr = 1;
    exec->hStderrThread =
      CreateThread(NULL, 0, pipe_reader_thread, ctx, 0, NULL);
    if (!exec->hStderrThread) {
      free(ctx);
      goto fail_process;
    }
  } else {
    exec->stderr_eof = 1;
  }

  /* ── Start timeout timer ── */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer =
      xEventLoopTimerAfter(exec->loop, on_timeout, exec, conf->timeout_ms);
  }

  /* ── Probe: child may have already exited ── */
  {
    DWORD exitCode;
    if (GetExitCodeProcess(exec->hProcess, &exitCode) &&
        exitCode != STILL_ACTIVE) {
      exec->child_exited     = 1;
      exec->result.exit_code = (int)exitCode;
      exec->result.signaled  = 0;
      cmd_check_completion(exec);
    }
  }

  exec->state = xCommandExecutorState_Running;
  return xErrno_Ok;

fail_process:
  /* Process was created but reader threads failed */
  TerminateProcess(exec->hProcess, 1);
  WaitForSingleObject(exec->hProcess, 5000);
  if (exec->hWait) {
    UnregisterWaitEx(exec->hWait, INVALID_HANDLE_VALUE);
    exec->hWait = NULL;
  }
  if (exec->hStdoutThread) {
    WaitForSingleObject(exec->hStdoutThread, 5000);
    CloseHandle(exec->hStdoutThread);
    exec->hStdoutThread = NULL;
  }
  if (exec->hStderrThread) {
    WaitForSingleObject(exec->hStderrThread, 5000);
    CloseHandle(exec->hStderrThread);
    exec->hStderrThread = NULL;
  }
  if (exec->hStdoutRead) {
    CloseHandle(exec->hStdoutRead);
    exec->hStdoutRead = NULL;
  }
  if (exec->hStderrRead) {
    CloseHandle(exec->hStderrRead);
    exec->hStderrRead = NULL;
  }
  if (exec->hStdinWrite) {
    CloseHandle(exec->hStdinWrite);
    exec->hStdinWrite = NULL;
  }
  if (exec->stdin_fd >= 0) {
    _close(exec->stdin_fd);
    exec->stdin_fd = -1;
  }
  if (exec->hProcess) {
    CloseHandle(exec->hProcess);
    exec->hProcess = NULL;
  }
  if (exec->hPrimaryThread) {
    CloseHandle(exec->hPrimaryThread);
    exec->hPrimaryThread = NULL;
  }
  goto fail;

fail:
  if (hStdinRead) CloseHandle(hStdinRead);
  if (hStdoutWrite) CloseHandle(hStdoutWrite);
  if (hStderrWrite) CloseHandle(hStderrWrite);
  if (exec->hStdinWrite) {
    CloseHandle(exec->hStdinWrite);
    exec->hStdinWrite = NULL;
  }
  if (exec->hStdoutRead) {
    CloseHandle(exec->hStdoutRead);
    exec->hStdoutRead = NULL;
  }
  if (exec->hStderrRead) {
    CloseHandle(exec->hStderrRead);
    exec->hStderrRead = NULL;
  }
  if (exec->stdin_fd >= 0) {
    _close(exec->stdin_fd);
    exec->stdin_fd = -1;
  }
  if (exec->stdout_buf) {
    xStringDestroy(exec->stdout_buf);
    exec->stdout_buf = NULL;
  }
  if (exec->stderr_buf) {
    xStringDestroy(exec->stderr_buf);
    exec->stderr_buf = NULL;
  }
  exec->state = xCommandExecutorState_Idle;
  return xErrno_SysError;
}

/* ═══════════════════════════════════════════════════════════════════
 * PTY mode execution via ConPTY
 * ═══════════════════════════════════════════════════════════════════ */

static xErrno xCommandExecutorSubmitPty(struct xCommandExecutor_ *exec,
                                        const xCommandConf       *conf) {
  /* ── Resolve ConPTY functions ── */
  if (!conpty_resolve()) return xErrno_NotSupported;

  /* ── Reset state ── */
  if (exec->stdout_buf) xStringDestroy(exec->stdout_buf);
  if (exec->stderr_buf) xStringDestroy(exec->stderr_buf);
  memset(&exec->result, 0, sizeof(exec->result));
  exec->stdout_buf     = NULL;
  exec->stderr_buf     = NULL;
  exec->stdout_max     = conf->stdout_cap;
  exec->stderr_max     = 0; /* stderr merged into stdout in PTY mode */
  exec->stdout_mode    = conf->stdout_mode;
  exec->stderr_mode    = xCommandOutput_Discard; /* ignored in PTY mode */
  exec->input_mode     = xCommandInput_Pty;
  exec->destroying     = 0;
  exec->force_killed   = 0;
  exec->stdout_eof     = 0;
  exec->stderr_eof     = 1; /* no separate stderr in PTY mode */
  exec->child_exited   = 0;
  exec->result.pty_fd  = -1;
  exec->stdin_fd       = -1;
  exec->hStdinWrite    = NULL;
  exec->hStdoutRead    = NULL;
  exec->hStderrRead    = NULL;
  exec->hWait          = NULL;
  exec->hConPty        = NULL;
  exec->hPtyConInWrite = NULL;
  exec->hPtyConOutRead = NULL;
  exec->pty_fd         = -1;
  exec->hStdoutThread  = NULL;
  exec->hStderrThread  = NULL;
  exec->hProcess       = NULL;
  exec->hPrimaryThread = NULL;
  exec->timeout_timer  = NULL;
  exec->cancel_timer   = NULL;

  if (conf->stdout_mode == xCommandOutput_Capture) {
    exec->stdout_buf = xStringCreate(NULL);
    if (!exec->stdout_buf) goto fail;
  }

  /* Callbacks (on_stdout, on_stderr, on_done, ud) were already set by
   * xCommandExecutorSubmit before calling us.  on_stderr is never invoked
   * in PTY mode (all output is merged through ConPTY). */

  /* ── Create ConPTY pipes ── */
  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

  /* ConPTY input pipe: parent writes → child (ConPTY) reads */
  HANDLE hConPtyInRead = NULL; /* child side (goes into CreatePseudoConsole) */
  if (!CreatePipe(&hConPtyInRead, &exec->hPtyConInWrite, &sa, 0)) goto fail;
  SetHandleInformation(exec->hPtyConInWrite, HANDLE_FLAG_INHERIT, 0);

  /* ConPTY output pipe: child (ConPTY) writes → parent reads */
  HANDLE hConPtyOutWrite = NULL; /* child side */
  if (!CreatePipe(&exec->hPtyConOutRead, &hConPtyOutWrite, &sa, 0)) {
    CloseHandle(hConPtyInRead);
    CloseHandle(exec->hPtyConInWrite);
    exec->hPtyConInWrite = NULL;
    goto fail;
  }
  SetHandleInformation(exec->hPtyConOutRead, HANDLE_FLAG_INHERIT, 0);

  /* ── Create pseudo console ── */
  COORD   conSize = {120, 30};
  HPCON   hPC     = NULL;
  HRESULT hr =
    fn_CreatePseudoConsole(conSize, hConPtyInRead, hConPtyOutWrite, 0, &hPC);
  /* After CreatePseudoConsole, the child-side handles belong to ConPTY.
   * Close our references — ConPTY has its own duplicates. */
  CloseHandle(hConPtyInRead);
  CloseHandle(hConPtyOutWrite);

  if (FAILED(hr) || !hPC) goto fail;

  exec->hConPty = hPC;

  /* ── Build startup info with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ── */
  SIZE_T attrListSize = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
  LPPROC_THREAD_ATTRIBUTE_LIST attrList =
    (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrListSize);
  if (!attrList) goto fail_conpty;

  if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize)) {
    HeapFree(GetProcessHeap(), 0, attrList);
    goto fail_conpty;
  }

  if (!UpdateProcThreadAttribute(attrList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
                                 sizeof(HPCON), NULL, NULL)) {
    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);
    goto fail_conpty;
  }

  STARTUPINFOEXW siex;
  memset(&siex, 0, sizeof(siex));
  siex.StartupInfo.cb         = sizeof(siex);
  siex.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
  siex.StartupInfo.hStdInput  = NULL;
  siex.StartupInfo.hStdOutput = NULL;
  siex.StartupInfo.hStdError  = NULL;
  siex.lpAttributeList        = attrList;

  /* ── Build command line ── */
  wchar_t *wcmdline = build_command_line(conf->cmd, conf->argv);
  if (!wcmdline) {
    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);
    goto fail_conpty;
  }

  /* ── Working directory ── */
  wchar_t *wcwd = NULL;
  if (conf->cwd) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, conf->cwd, -1, NULL, 0);
    wcwd     = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (wcwd) MultiByteToWideChar(CP_UTF8, 0, conf->cwd, -1, wcwd, wlen);
  }

  /* ── Create process ── */
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));

  exec->start_ms = xMonoMs();

  BOOL ok =
    CreateProcessW(NULL,     /* lpApplicationName */
                   wcmdline, /* lpCommandLine */
                   NULL,     /* lpProcessAttributes */
                   NULL,     /* lpThreadAttributes */
                   FALSE,    /* bInheritHandles — FALSE for ConPTY */
                   CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                   NULL,              /* lpEnvironment */
                   wcwd,              /* lpCurrentDirectory */
                   &siex.StartupInfo, /* lpStartupInfo */
                   &pi                /* lpProcessInformation */
    );

  free(wcmdline);
  free(wcwd);
  DeleteProcThreadAttributeList(attrList);
  HeapFree(GetProcessHeap(), 0, attrList);

  if (!ok) goto fail_conpty;

  exec->hProcess       = pi.hProcess;
  exec->hPrimaryThread = pi.hThread;
  exec->child_pid      = pi.dwProcessId;

  /* Convert ConPTY input write handle to CRT fd (for PtyFd / StdinFd) */
  exec->pty_fd =
    _open_osfhandle((intptr_t)exec->hPtyConInWrite, _O_WRONLY | _O_BINARY);
  if (exec->pty_fd >= 0) {
    exec->hPtyConInWrite = NULL; /* CRT owns it now */
    exec->result.pty_fd  = exec->pty_fd;
  }

  /* ── Register process exit wait ── */
  if (!RegisterWaitForSingleObject(&exec->hWait, exec->hProcess,
                                   on_process_exit, exec, INFINITE,
                                   WT_EXECUTEONLYONCE)) {
    exec->hWait = NULL;
  }

  /* ── Start ConPTY output reader thread ── */
  if (conf->stdout_mode != xCommandOutput_Discard) {
    struct PipeReaderCtx_ *ctx = (struct PipeReaderCtx_ *)malloc(sizeof(*ctx));
    if (!ctx) goto fail_process;
    ctx->hRead     = exec->hPtyConOutRead;
    ctx->exec      = exec;
    ctx->is_stderr = 0; /* all output is merged through ConPTY */
    exec->hStdoutThread =
      CreateThread(NULL, 0, pipe_reader_thread, ctx, 0, NULL);
    if (!exec->hStdoutThread) {
      free(ctx);
      goto fail_process;
    }
  } else {
    /* In Discard mode, we still need to drain the ConPTY output to prevent
     * the child from blocking on write.  Start a reader that discards. */
    struct PipeReaderCtx_ *ctx = (struct PipeReaderCtx_ *)malloc(sizeof(*ctx));
    if (!ctx) goto fail_process;
    ctx->hRead     = exec->hPtyConOutRead;
    ctx->exec      = exec;
    ctx->is_stderr = 0;
    exec->hStdoutThread =
      CreateThread(NULL, 0, pipe_reader_thread, ctx, 0, NULL);
    if (!exec->hStdoutThread) {
      free(ctx);
      goto fail_process;
    }
  }

  /* ── Start timeout timer ── */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer =
      xEventLoopTimerAfter(exec->loop, on_timeout, exec, conf->timeout_ms);
  }

  /* ── Probe: child may have already exited ── */
  {
    DWORD exitCode;
    if (GetExitCodeProcess(exec->hProcess, &exitCode) &&
        exitCode != STILL_ACTIVE) {
      exec->child_exited     = 1;
      exec->result.exit_code = (int)exitCode;
      exec->result.signaled  = 0;
      cmd_check_completion(exec);
    }
  }

  exec->state = xCommandExecutorState_Running;
  return xErrno_Ok;

fail_process:
  TerminateProcess(exec->hProcess, 1);
  WaitForSingleObject(exec->hProcess, 5000);
  if (exec->hWait) {
    UnregisterWaitEx(exec->hWait, INVALID_HANDLE_VALUE);
    exec->hWait = NULL;
  }
  if (exec->hStdoutThread) {
    WaitForSingleObject(exec->hStdoutThread, 5000);
    CloseHandle(exec->hStdoutThread);
    exec->hStdoutThread = NULL;
  }
  if (exec->hPtyConOutRead) {
    CloseHandle(exec->hPtyConOutRead);
    exec->hPtyConOutRead = NULL;
  }
  if (exec->hPtyConInWrite) {
    CloseHandle(exec->hPtyConInWrite);
    exec->hPtyConInWrite = NULL;
  }
  if (exec->pty_fd >= 0) {
    _close(exec->pty_fd);
    exec->pty_fd = -1;
  }
  if (exec->hProcess) {
    CloseHandle(exec->hProcess);
    exec->hProcess = NULL;
  }
  if (exec->hPrimaryThread) {
    CloseHandle(exec->hPrimaryThread);
    exec->hPrimaryThread = NULL;
  }
  if (exec->hConPty) {
    fn_ClosePseudoConsole(exec->hConPty);
    exec->hConPty = NULL;
  }
  goto fail;

fail_conpty:
  if (exec->hPtyConOutRead) {
    CloseHandle(exec->hPtyConOutRead);
    exec->hPtyConOutRead = NULL;
  }
  if (exec->hPtyConInWrite) {
    CloseHandle(exec->hPtyConInWrite);
    exec->hPtyConInWrite = NULL;
  }
  if (exec->hConPty) {
    fn_ClosePseudoConsole(exec->hConPty);
    exec->hConPty = NULL;
  }

fail:
  if (exec->stdout_buf) {
    xStringDestroy(exec->stdout_buf);
    exec->stdout_buf = NULL;
  }
  if (exec->stderr_buf) {
    xStringDestroy(exec->stderr_buf);
    exec->stderr_buf = NULL;
  }
  exec->state = xCommandExecutorState_Idle;
  return xErrno_SysError;
}

/* ═══════════════════════════════════════════════════════════════════
 * Cancel
 * ═══════════════════════════════════════════════════════════════════ */

xErrno xCommandExecutorCancel(xCommandExecutor exec_) {
  if (!exec_) return xErrno_InvalidArg;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;

  if (exec->state != xCommandExecutorState_Running) return xErrno_InvalidState;

  exec->force_killed     = 1;
  exec->result.timed_out = 1;
  exec->state            = xCommandExecutorState_Cancelling;

  /* Kill the child process immediately */
  if (exec->hProcess) TerminateProcess(exec->hProcess, 1);

  /* Grace timer: retry if the first TerminateProcess didn't take effect */
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                            CMD_CANCEL_GRACE_MS);
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Query
 * ═══════════════════════════════════════════════════════════════════ */

int xCommandExecutorPid(xCommandExecutor exec_) {
  if (!exec_) return -1;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;
  return (exec->state != xCommandExecutorState_Idle) ? (int)exec->child_pid
                                                     : -1;
}

int xCommandExecutorIsRunning(xCommandExecutor exec_) {
  if (!exec_) return 0;
  return ((struct xCommandExecutor_ *)exec_)->state !=
         xCommandExecutorState_Idle;
}

int xCommandExecutorPtyFd(xCommandExecutor exec_) {
  if (!exec_) return -1;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;
  if (exec->state == xCommandExecutorState_Idle) return -1;
  /* PTY mode: return the ConPTY input write fd */
  if (exec->input_mode == xCommandInput_Pty && exec->pty_fd >= 0)
    return exec->pty_fd;
  return -1;
}

int xCommandExecutorStdinFd(xCommandExecutor exec_) {
  if (!exec_) return -1;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;
  if (exec->state == xCommandExecutorState_Idle) return -1;
  /* PTY mode: write to the ConPTY input (same as PtyFd) */
  if (exec->input_mode == xCommandInput_Pty && exec->pty_fd >= 0)
    return exec->pty_fd;
  /* Pipe mode: write to the stdin pipe write end */
  return exec->stdin_fd;
}

#endif /* _WIN32 */
