/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * command.c - Async command executor over xEventLoop
 */

#include <xbase/command.h>
#include <xbase/string.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef XK_HAS_PTY
#ifdef XK_HAS_UTIL_H
#include <util.h> /* forkpty on macOS */
#elif defined(XK_HAS_PTY_H)
#include <pty.h> /* forkpty on Linux */
#endif
#endif

/* ───────────────────── Constants ───────────────────── */

#define CMD_CANCEL_GRACE_MS 5000 /**< SIGTERM → SIGKILL grace period */
#define CMD_READ_BUF_SIZE   4096 /**< Per-read buffer size           */

/* ───────────────────── Internal state ───────────────────── */

enum xCommandExecutorState_ {
  xCommandExecutorState_Idle = 0,
  xCommandExecutorState_Running,
  xCommandExecutorState_Cancelling, /**< SIGTERM sent, waiting for exit or
                                       SIGKILL */
};

struct xCommandExecutor_ {
  xEventLoop loop;

  /* Child process */
  pid_t                       child_pid;
  enum xCommandExecutorState_ state;

  /* Pipes: [0] = read end (parent), [1] = write end (child) */
  int stdout_pipe[2];
  int stderr_pipe[2];

  /* PTY master fd (valid in PTY mode, -1 otherwise) */
  int pty_master_fd;

  /* Stdin pipe: [0] = read end (child), [1] = write end (parent).
   * Only used in Pipe mode. [0] is dup'd into child's STDIN_FILENO;
   * [1] is exposed via xCommandExecutorStdinFd(). */
  int stdin_pipe[2];

  /* Event sources for pipe read ends / PTY master */
  xEventSource stdout_src;
  xEventSource stderr_src;

  /* Timeout timer */
  xEventTimer timeout_timer;

  /* Cancel grace timer (SIGTERM → SIGKILL) */
  xEventTimer cancel_timer;

  /* Output capture buffers (Capture mode) */
  xString stdout_buf;
  xString stderr_buf;

  /* Configuration limits */
  size_t stdout_max; /**< from xCommandConf::stdout_cap */
  size_t stderr_max; /**< from xCommandConf::stderr_cap */

  /* Output modes (saved from xCommandConf) */
  xCommandOutputMode stdout_mode;
  xCommandOutputMode stderr_mode;

  /* Input mode */
  xCommandInputMode input_mode;

  /* Result (filled incrementally, finalized on exit) */
  xCommandResult result;

  /* Callbacks */
  xCommandExecutorOutputFunc on_stdout;
  xCommandExecutorOutputFunc on_stderr;
  xCommandExecutorDoneFunc   on_done;
  void                      *ud;

  /* Timing */
  uint64_t start_ms;

  /* Track which pipes are still open */
  int stdout_eof;
  int stderr_eof;
  int child_exited;

  /* Linked list for SIGCHLD multiplexer */
  struct xCommandExecutor_ *next;
};

/* ───────────────────── SIGCHLD multiplexer ───────────────────── */

/*
 * Multiple xCommandExecutor instances may be active on the same event loop.
 * We maintain a singly-linked list of running executors and
 * register SIGCHLD once. When SIGCHLD fires we walk the list
 * and call waitpid(WNOHANG) for each.
 */

static struct xCommandExecutor_ *g_sigchld_head  = NULL;
static int                       g_sigchld_count = 0;

/* No-op signal handler so that SIGCHLD is delivered to kqueue/epoll.
 * SIG_IGN causes kqueue to not receive EVFILT_SIGNAL events. */
static void sigchld_handler(int signo, void *arg);

static int sigchld_register(xEventLoop loop) {
  if (g_sigchld_count == 0) {
    xErrno err = xEventLoopSignalWatch(loop, SIGCHLD, sigchld_handler, NULL);
    if (err != xErrno_Ok) return -1;
  }
  g_sigchld_count++;
  return 0;
}

static void sigchld_unregister(xEventLoop loop) {
  g_sigchld_count--;
  if (g_sigchld_count == 0) {
    xEventLoopSignalWatch(loop, SIGCHLD, NULL, NULL);
  }
}

static void sigchld_add(struct xCommandExecutor_ *exec) {
  exec->next     = g_sigchld_head;
  g_sigchld_head = exec;
}

static void sigchld_remove(struct xCommandExecutor_ *exec) {
  struct xCommandExecutor_ **pp = &g_sigchld_head;
  while (*pp) {
    if (*pp == exec) {
      *pp        = exec->next;
      exec->next = NULL;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* ───────────────────── Forward declarations ───────────────────── */

static void on_stdout_readable(int fd, xEventMask mask, void *arg);
static void on_stderr_readable(int fd, xEventMask mask, void *arg);
#ifdef XK_HAS_PTY
static void on_pty_readable(int fd, xEventMask mask, void *arg);
#endif
static void on_timeout(void *arg);
static void on_cancel_grace(void *arg);
static void cmd_check_completion(struct xCommandExecutor_ *exec);
static void cmd_fire_done(struct xCommandExecutor_ *exec);
static void cmd_cleanup(struct xCommandExecutor_ *exec);
static void cmd_kill_pg(struct xCommandExecutor_ *exec, int sig);
#ifdef XK_HAS_PTY
static xErrno xCommandExecutorSubmitPty(struct xCommandExecutor_ *exec,
                                        const xCommandConf       *conf);
#endif

/* ───────────────────── Pipe helpers ───────────────────── */

/**
 * Create a pipe with both ends set O_CLOEXEC + O_NONBLOCK.
 * Returns 0 on success, -1 on failure.
 */
static int pipe_cloexec_nonblock(int fds[2]) {
#if defined(__linux__) && defined(__NR_pipe2)
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) return 0;
  /* Fall through to manual approach on pipe2 failure */
#endif

  if (pipe(fds) != 0) return -1;

  for (int i = 0; i < 2; i++) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    int fdflags = fcntl(fds[i], F_GETFD, 0);
    if (fdflags < 0 || fcntl(fds[i], F_SETFD, fdflags | FD_CLOEXEC) < 0)
      goto fail;
  }
  return 0;

fail:
  close(fds[0]);
  close(fds[1]);
  return -1;
}

/* ───────────────────── PTY helpers ───────────────────── */

#ifdef XK_HAS_PTY

/**
 * Set a fd to non-blocking mode.
 * Returns 0 on success, -1 on failure.
 */
static int fd_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif /* XK_HAS_PTY */

/* ───────────────────── Lifecycle ───────────────────── */

xCommandExecutor xCommandExecutorCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xCommandExecutor_ *exec =
    (struct xCommandExecutor_ *)calloc(1, sizeof(*exec));
  if (!exec) return NULL;

  exec->loop           = loop;
  exec->child_pid      = -1;
  exec->stdout_pipe[0] = -1;
  exec->stdout_pipe[1] = -1;
  exec->stderr_pipe[0] = -1;
  exec->stderr_pipe[1] = -1;
  exec->pty_master_fd  = -1;
  exec->stdin_pipe[0]  = -1;
  exec->stdin_pipe[1]  = -1;
  exec->stdout_src     = NULL;
  exec->stderr_src     = NULL;
  exec->timeout_timer  = NULL;
  exec->cancel_timer   = NULL;
  exec->state          = xCommandExecutorState_Idle;
  exec->stdout_buf     = NULL;
  exec->stderr_buf     = NULL;

  return (xCommandExecutor)exec;
}

void xCommandExecutorDestroy(xCommandExecutor exec_) {
  if (!exec_) return;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;

  if (exec->state != xCommandExecutorState_Idle) {
    /* Kill the child process group immediately */
    cmd_kill_pg(exec, SIGKILL);
    if (exec->child_pid > 0) {
      /* Reap zombie (blocking — child was already sent SIGKILL) */
      int status;
      waitpid(exec->child_pid, &status, 0);
    }
    sigchld_remove(exec);
    sigchld_unregister(exec->loop);
    cmd_cleanup(exec);
  }

  if (exec->stdout_buf) xStringDestroy(exec->stdout_buf);
  if (exec->stderr_buf) xStringDestroy(exec->stderr_buf);
  free(exec);
}

/* ───────────────────── Child argv builder ───────────────────── */

/**
 * Build the argv array for execvp/execve.
 * Returns a malloc'd array, or NULL on failure.
 * Caller must free().
 */
static const char **build_exec_argv(const char *cmd, const char **user_argv) {
  int argc = 1; /* cmd itself */
  if (user_argv) {
    while (user_argv[argc - 1])
      argc++;
  }
  const char **exec_argv =
    (const char **)malloc((argc + 1) * sizeof(const char *));
  if (!exec_argv) return NULL;
  exec_argv[0] = cmd;
  if (user_argv) {
    for (int i = 1; i < argc; i++)
      exec_argv[i] = user_argv[i - 1];
  }
  exec_argv[argc] = NULL;
  return exec_argv;
}

/* ───────────────────── Execution ───────────────────── */

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
  exec->stdout_buf  = NULL;
  exec->stderr_buf  = NULL;
  exec->stdout_max  = conf->stdout_cap;
  exec->stderr_max  = conf->stderr_cap;
  exec->stdout_mode = conf->stdout_mode;
  exec->stderr_mode = conf->stderr_mode;
  exec->input_mode  = conf->input_mode;

  /* Create capture buffers for Capture mode */
  if (conf->stdout_mode == xCommandOutput_Capture) {
    exec->stdout_buf = xStringCreate(NULL);
    if (!exec->stdout_buf) goto fail;
  }
  if (conf->stderr_mode == xCommandOutput_Capture &&
      conf->input_mode != xCommandInput_Pty) {
    exec->stderr_buf = xStringCreate(NULL);
    if (!exec->stderr_buf) goto fail;
  }
  exec->stdout_eof    = 0;
  exec->stderr_eof    = 0;
  exec->child_exited  = 0;
  exec->pty_master_fd = -1;
  exec->result.pty_fd = -1;
  exec->stdin_pipe[0] = -1;
  exec->stdin_pipe[1] = -1;

  exec->on_stdout = on_stdout;
  exec->on_stderr = on_stderr;
  exec->on_done   = on_done;
  exec->ud        = ud;

  /* ── PTY mode ── */
  if (conf->input_mode == xCommandInput_Pty) {
#ifdef XK_HAS_PTY
    return xCommandExecutorSubmitPty(exec, conf);
#else
    return xErrno_NotSupported;
#endif
  }

  /* ── Pipe mode (default) ── */

  /* ── Create pipes ── */
  /* Stdin pipe: parent writes to [1], child reads from [0] */
  if (pipe_cloexec_nonblock(exec->stdin_pipe) != 0) goto fail;
  /* Make the write end non-blocking so writes don't block the event loop */
  fd_set_nonblock(exec->stdin_pipe[1]);

  if (conf->stdout_mode != xCommandOutput_Discard) {
    if (pipe_cloexec_nonblock(exec->stdout_pipe) != 0) goto fail;
  }
  if (conf->stderr_mode != xCommandOutput_Discard) {
    if (pipe_cloexec_nonblock(exec->stderr_pipe) != 0) goto fail_pipes;
  }

  /* Save pipe fds for child (before fork, to avoid race) */
  int child_stdout_wfd = exec->stdout_pipe[1];
  int child_stderr_wfd = exec->stderr_pipe[1];

  /* Register SIGCHLD BEFORE fork so we don't miss the signal if the
   * child exits before we get a chance to call xEventLoopSignalWatch. */
  if (sigchld_register(exec->loop) != 0) goto fail_pipes;
  sigchld_add(exec);

  /* ── Fork ── */
  exec->start_ms = xMonoMs();
  pid_t pid      = fork();
  if (pid < 0) {
    sigchld_remove(exec);
    sigchld_unregister(exec->loop);
    goto fail_pipes;
  }

  if (pid == 0) {
    /* ── Child process ── */

    /* Create own process group for killpg() support */
    setpgid(0, 0);

    /* Redirect stdin from pipe */
    if (exec->stdin_pipe[0] >= 0) {
      dup2(exec->stdin_pipe[0], STDIN_FILENO);
    }

    /* Redirect stdout */
    if (conf->stdout_mode == xCommandOutput_Discard) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        close(devnull);
      }
    } else {
      dup2(child_stdout_wfd, STDOUT_FILENO);
    }

    /* Redirect stderr */
    if (conf->stderr_mode == xCommandOutput_Discard) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    } else {
      dup2(child_stderr_wfd, STDERR_FILENO);
    }

    /* Close all pipe fds (write ends are now dup'd to stdout/stderr) */
    close(exec->stdin_pipe[0]);
    close(exec->stdin_pipe[1]);
    close(exec->stdout_pipe[0]);
    close(exec->stdout_pipe[1]);
    close(exec->stderr_pipe[0]);
    close(exec->stderr_pipe[1]);

    /* Clear O_NONBLOCK on stdin/stdout/stderr.
     *
     * pipe_cloexec_nonblock() sets O_NONBLOCK on both ends of each
     * pipe.  Since O_NONBLOCK is a file-description-level flag,
     * dup2() preserves it on the new fd.  Without clearing it,
     * the child's stdin would be non-blocking — causing reads to
     * return EAGAIN immediately, which Python's input() interprets
     * as EOF.  Clear it on all three standard fds for safety,
     * though stdin is the one that breaks in practice. */
    {
      int std_fds[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
      for (int i = 0; i < 3; i++) {
        int fl = fcntl(std_fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(std_fds[i], F_SETFL, fl & ~O_NONBLOCK);
      }
    }

    /* Change working directory */
    if (conf->cwd) {
      if (chdir(conf->cwd) != 0) _exit(127);
    }

    /* Build argv for execvp */
    const char **exec_argv = build_exec_argv(conf->cmd, conf->argv);
    if (!exec_argv) _exit(127);

    /* Execute */
    if (conf->envp) {
      execve(conf->cmd, (char *const *)exec_argv, (char *const *)conf->envp);
    } else {
      execvp(conf->cmd, (char *const *)exec_argv);
    }
    _exit(127); /* exec failed */
  }

  /* ── Parent process ── */
  exec->child_pid = pid;

  /* Synchronize process group creation to avoid race with child's setpgid(0,0).
   */
  setpgid(pid, pid);

  /* Close stdin pipe read end (child owns it now) */
  if (exec->stdin_pipe[0] >= 0) {
    close(exec->stdin_pipe[0]);
    exec->stdin_pipe[0] = -1;
  }

  /* Close write ends (child owns them now) */
  if (exec->stdout_pipe[1] >= 0) {
    close(exec->stdout_pipe[1]);
    exec->stdout_pipe[1] = -1;
  }
  if (exec->stderr_pipe[1] >= 0) {
    close(exec->stderr_pipe[1]);
    exec->stderr_pipe[1] = -1;
  }

  /* Probe: the child may have already exited before we registered
   * event sources.  Do a non-blocking waitpid to catch this race. */
  {
    int   status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      exec->child_exited = 1;
      if (WIFEXITED(status)) {
        exec->result.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exec->result.signaled = WTERMSIG(status);
      }
    }
  }

  /* Watch read ends for output */
  if (conf->stdout_mode != xCommandOutput_Discard &&
      exec->stdout_pipe[0] >= 0) {
    exec->stdout_src = xEventAdd(exec->loop, exec->stdout_pipe[0], xEvent_Read,
                                 on_stdout_readable, exec);
    if (!exec->stdout_src) goto fail_sigchld;
  } else {
    exec->stdout_eof = 1;
  }

  if (conf->stderr_mode != xCommandOutput_Discard &&
      exec->stderr_pipe[0] >= 0) {
    exec->stderr_src = xEventAdd(exec->loop, exec->stderr_pipe[0], xEvent_Read,
                                 on_stderr_readable, exec);
    if (!exec->stderr_src) goto fail_sigchld;
  } else {
    exec->stderr_eof = 1;
  }

  /* Start timeout timer */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer =
      xEventLoopTimerAfter(exec->loop, on_timeout, exec, conf->timeout_ms);
  }

  exec->state = xCommandExecutorState_Running;
  return xErrno_Ok;

fail_sigchld:
  sigchld_remove(exec);
  sigchld_unregister(exec->loop);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
fail_pipes:
  if (exec->stdin_pipe[0] >= 0) close(exec->stdin_pipe[0]);
  if (exec->stdin_pipe[1] >= 0) close(exec->stdin_pipe[1]);
  exec->stdin_pipe[0] = -1;
  exec->stdin_pipe[1] = -1;
  if (exec->stdout_pipe[0] >= 0) close(exec->stdout_pipe[0]);
  if (exec->stdout_pipe[1] >= 0) close(exec->stdout_pipe[1]);
  if (exec->stderr_pipe[0] >= 0) close(exec->stderr_pipe[0]);
  if (exec->stderr_pipe[1] >= 0) close(exec->stderr_pipe[1]);
  exec->stdout_pipe[0] = -1;
  exec->stdout_pipe[1] = -1;
  exec->stderr_pipe[0] = -1;
  exec->stderr_pipe[1] = -1;
fail:
  exec->state = xCommandExecutorState_Idle;
  return xErrno_SysError;
}

/* ───────────────────── PTY mode execution ───────────────────── */

#ifdef XK_HAS_PTY

static xErrno xCommandExecutorSubmitPty(struct xCommandExecutor_ *exec,
                                        const xCommandConf       *conf) {
  /* In PTY mode:
   * - We use forkpty() to create a PTY master/slave pair and fork.
   * - The child gets the slave side as its controlling terminal.
   * - stdin/stdout/stderr of the child all connect to the slave PTY.
   * - The parent reads from the master fd.
   * - stderr_mode is ignored (all output merged through PTY).
   * - stdout_mode controls how the merged output is handled.
   */

  /* If stdout is discarded and we're in PTY mode, there's no point
   * reading from the master fd.  We still allocate a PTY for the
   * child's sake (so it thinks it has a terminal), but we don't
   * watch the master fd. */

  /* Build argv before fork (to avoid malloc in child) */
  const char **exec_argv = build_exec_argv(conf->cmd, conf->argv);
  if (!exec_argv) goto fail;

  /* Register SIGCHLD BEFORE fork */
  if (sigchld_register(exec->loop) != 0) {
    free((void *)exec_argv);
    goto fail;
  }
  sigchld_add(exec);

  exec->start_ms = xMonoMs();

  int   master_fd = -1;
  pid_t pid;

#if defined(__APPLE__) || defined(__linux__)
  /* forkpty() is available on both macOS and Linux */
  pid = forkpty(&master_fd, NULL, NULL, NULL);
#else
  /* Fallback: use posix_openpt() + fork() */
  /* This path is for other POSIX systems; currently not needed. */
  pid   = -1;
  errno = ENOTSUP;
#endif

  if (pid < 0) {
    free((void *)exec_argv);
    sigchld_remove(exec);
    sigchld_unregister(exec->loop);
    goto fail;
  }

  if (pid == 0) {
    /* ── Child process ── */

    /* forkpty() already calls setsid() and makes the child a session
     * leader with its own process group (pgid == pid) whose controlling
     * terminal is the slave PTY.  Calling setpgid(0, 0) again here is
     * redundant and, on Linux, can detach the child from its controlling
     * terminal, causing writes to stdout/stderr to fail with EIO and
     * the process to exit with status 1 before exec completes.
     * See: https://man7.org/linux/man-pages/man3/forkpty.3.html */

    /* Change working directory */
    if (conf->cwd) {
      if (chdir(conf->cwd) != 0) _exit(127);
    }

    /* Execute */
    if (conf->envp) {
      execve(conf->cmd, (char *const *)exec_argv, (char *const *)conf->envp);
    } else {
      execvp(conf->cmd, (char *const *)exec_argv);
    }
    _exit(127); /* exec failed */
  }

  /* ── Parent process ── */
  free((void *)exec_argv);

  exec->child_pid     = pid;
  exec->pty_master_fd = master_fd;
  exec->result.pty_fd = master_fd;

  /* No setpgid() sync needed: forkpty() already placed the child in its
   * own process group (pgid == pid == session id). killpg(pid, ...) will
   * therefore target the whole child session as intended. */

  /* Set master fd to non-blocking for event loop integration */
  if (fd_set_nonblock(master_fd) != 0) {
    goto fail_sigchld;
  }

  /* In PTY mode, we always mark stderr as EOF since it's merged */
  exec->stderr_eof = 1;

  /* Watch master fd for output.
   * IMPORTANT: Register the event source BEFORE the waitpid probe below.
   * In edge-triggered epoll mode, if the child exits before we register,
   * the readable event on the master fd (EIO from slave close) is lost
   * and on_pty_readable will never fire, leaving stdout_eof = 0 forever.
   *
   * We must register even in Discard mode: reading from the master fd
   * drains the PTY buffer and ensures the child doesn't block on write.
   * The data is simply discarded in on_pty_readable when mode == Discard. */
  exec->stdout_src =
    xEventAdd(exec->loop, master_fd, xEvent_Read, on_pty_readable, exec);
  if (!exec->stdout_src) goto fail_sigchld;

  /* Probe: the child may have already exited */
  {
    int   status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      exec->child_exited = 1;
      if (WIFEXITED(status)) {
        exec->result.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exec->result.signaled = WTERMSIG(status);
      }
      /* If we haven't read any output yet, do a non-blocking read now.
       * For fast-exiting children (e.g. /bin/echo), the PTY slave may have
       * already closed before the event source was registered, so the
       * edge-triggered epoll won't fire.  Drain the pending data here to
       * avoid hanging. */
      if (!exec->stdout_eof) {
        char buf[CMD_READ_BUF_SIZE];
        for (;;) {
          ssize_t n = read(master_fd, buf, sizeof(buf));
          if (n > 0) {
            if (exec->stdout_mode == xCommandOutput_Capture) {
              xStringAppendLen(&exec->stdout_buf, buf, (size_t)n);
            } else if (exec->stdout_mode == xCommandOutput_Stream &&
                       exec->on_stdout) {
              exec->on_stdout((xCommandExecutor)exec, buf, (size_t)n, exec->ud);
            }
          } else if (n == 0) {
            /* EOF */
            xEventDel(exec->loop, exec->stdout_src);
            exec->stdout_src = NULL;
            exec->stdout_eof = 1;
            break;
          } else {
            if (errno == EIO) {
              /* Slave closed — treat as EOF for PTY */
              xEventDel(exec->loop, exec->stdout_src);
              exec->stdout_src = NULL;
              exec->stdout_eof = 1;
              break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              /* No more data for now; the event source will notify later */
              break;
            }
            /* Other error — treat as EOF */
            xEventDel(exec->loop, exec->stdout_src);
            exec->stdout_src = NULL;
            exec->stdout_eof = 1;
            break;
          }
        }
      }
      cmd_check_completion(exec);
    }
  }

  /* Start timeout timer */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer =
      xEventLoopTimerAfter(exec->loop, on_timeout, exec, conf->timeout_ms);
  }

  exec->state = xCommandExecutorState_Running;
  return xErrno_Ok;

fail_sigchld:
  sigchld_remove(exec);
  sigchld_unregister(exec->loop);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  if (master_fd >= 0) {
    close(master_fd);
    exec->pty_master_fd = -1;
    exec->result.pty_fd = -1;
  }
fail:
  exec->state = xCommandExecutorState_Idle;
  return xErrno_SysError;
}

#endif /* XK_HAS_PTY */

/* ───────────────────── Cancel ───────────────────── */

xErrno xCommandExecutorCancel(xCommandExecutor exec_) {
  if (!exec_) return xErrno_InvalidArg;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;

  if (exec->state != xCommandExecutorState_Running) return xErrno_InvalidState;

  /* Send SIGTERM to the process group */
  cmd_kill_pg(exec, SIGTERM);
  exec->result.timed_out = 1;
  exec->state            = xCommandExecutorState_Cancelling;

  /* Start grace timer for SIGKILL */
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                            CMD_CANCEL_GRACE_MS);
  return xErrno_Ok;
}

/* ───────────────────── Query ───────────────────── */

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
  return exec->pty_master_fd;
}

int xCommandExecutorStdinFd(xCommandExecutor exec_) {
  if (!exec_) return -1;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)exec_;
  if (exec->state == xCommandExecutorState_Idle) return -1;
  /* PTY mode: write to the master fd */
  if (exec->input_mode == xCommandInput_Pty && exec->pty_master_fd >= 0)
    return exec->pty_master_fd;
  /* Pipe mode: write to the stdin pipe write end */
  if (exec->stdin_pipe[1] >= 0) return exec->stdin_pipe[1];
  return -1;
}

/* ───────────────────── Pipe read callbacks ───────────────────── */

static void on_stdout_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  char                      buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (exec->stdout_mode == xCommandOutput_Capture) {
        xStringAppendLen(&exec->stdout_buf, buf, (size_t)n);
      } else if (exec->stdout_mode == xCommandOutput_Stream &&
                 exec->on_stdout) {
        exec->on_stdout((xCommandExecutor)exec, buf, (size_t)n, exec->ud);
      }
    } else if (n == 0) {
      /* EOF */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      close(exec->stdout_pipe[0]);
      exec->stdout_pipe[0] = -1;
      exec->stdout_eof     = 1;
      cmd_check_completion(exec);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      /* Error — treat as EOF */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      close(exec->stdout_pipe[0]);
      exec->stdout_pipe[0] = -1;
      exec->stdout_eof     = 1;
      cmd_check_completion(exec);
      break;
    }
  }
}

static void on_stderr_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  char                      buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (exec->stderr_mode == xCommandOutput_Capture) {
        xStringAppendLen(&exec->stderr_buf, buf, (size_t)n);
      } else if (exec->stderr_mode == xCommandOutput_Stream &&
                 exec->on_stderr) {
        exec->on_stderr((xCommandExecutor)exec, buf, (size_t)n, exec->ud);
      }
    } else if (n == 0) {
      xEventDel(exec->loop, exec->stderr_src);
      exec->stderr_src = NULL;
      close(exec->stderr_pipe[0]);
      exec->stderr_pipe[0] = -1;
      exec->stderr_eof     = 1;
      cmd_check_completion(exec);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      xEventDel(exec->loop, exec->stderr_src);
      exec->stderr_src = NULL;
      close(exec->stderr_pipe[0]);
      exec->stderr_pipe[0] = -1;
      exec->stderr_eof     = 1;
      cmd_check_completion(exec);
      break;
    }
  }
}

/* ───────────────────── PTY read callback ───────────────────── */

#ifdef XK_HAS_PTY

static void on_pty_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  char                      buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      /* In PTY mode, all output is merged and treated as stdout */
      if (exec->stdout_mode == xCommandOutput_Capture) {
        xStringAppendLen(&exec->stdout_buf, buf, (size_t)n);
      } else if (exec->stdout_mode == xCommandOutput_Stream &&
                 exec->on_stdout) {
        exec->on_stdout((xCommandExecutor)exec, buf, (size_t)n, exec->ud);
      }
      /* Discard mode: read and discard to drain the PTY buffer */
    } else if (n == 0) {
      /* EOF — master side closed means child exited */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      /* Don't close pty_master_fd here — it will be closed in cmd_cleanup.
       * Also, don't close it prematurely because we may want to write to it. */
      exec->stdout_eof = 1;
      cmd_check_completion(exec);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      /* EIO is commonly returned when the slave side is closed (child exited)
       */
      if (errno == EIO) {
        xEventDel(exec->loop, exec->stdout_src);
        exec->stdout_src = NULL;
        exec->stdout_eof = 1;
        cmd_check_completion(exec);
        break;
      }
      /* Other error — treat as EOF */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      exec->stdout_eof = 1;
      cmd_check_completion(exec);
      break;
    }
  }
}

#endif /* XK_HAS_PTY */

/* ───────────────────── Timeout callback ───────────────────── */

static void on_timeout(void *arg) {
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  exec->timeout_timer            = NULL;

  if (exec->state != xCommandExecutorState_Running) return;

  /* Send SIGTERM, then schedule SIGKILL */
  cmd_kill_pg(exec, SIGTERM);
  exec->result.timed_out = 1;
  exec->state            = xCommandExecutorState_Cancelling;
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                            CMD_CANCEL_GRACE_MS);
}

/* ───────────────────── Cancel grace period ───────────────────── */

static void on_cancel_grace(void *arg) {
  struct xCommandExecutor_ *exec = (struct xCommandExecutor_ *)arg;
  exec->cancel_timer             = NULL;

  if (exec->state != xCommandExecutorState_Cancelling) return;

  /* Child didn't exit after SIGTERM — force kill */
  cmd_kill_pg(exec, SIGKILL);
  /* SIGCHLD handler will reap and complete */
}

/* ───────────────────── SIGCHLD handler ───────────────────── */

static void sigchld_handler(int signo, void *arg) {
  (void)signo;
  (void)arg;

  /* Walk all registered executors and try to reap. */
  struct xCommandExecutor_ *cur = g_sigchld_head;
  while (cur) {
    struct xCommandExecutor_ *exec = cur;
    if (exec->child_pid > 0 && !exec->child_exited) {
      int   status;
      pid_t ret = waitpid(exec->child_pid, &status, WNOHANG);
      if (ret == exec->child_pid) {
        exec->child_exited = 1;
        if (WIFEXITED(status)) {
          exec->result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          exec->result.signaled = WTERMSIG(status);
        }
        cmd_check_completion(exec);
      } else if (ret < 0 && errno == ECHILD) {
        /* Child was auto-reaped by kernel (SIG_IGN).  We cannot know
         * the exit status, so assume success unless already set. */
        exec->child_exited = 1;
        cmd_check_completion(exec);
      }
    }
    cur = exec->next;
  }
}

/* ───────────────────── Completion check ───────────────────── */

static void cmd_check_completion(struct xCommandExecutor_ *exec) {
  /* All three conditions must be met: stdout EOF, stderr EOF, child exited */
  if (!exec->stdout_eof || !exec->stderr_eof || !exec->child_exited) return;

  cmd_fire_done(exec);
}

/* ───────────────────── Fire completion ───────────────────── */

static void cmd_fire_done(struct xCommandExecutor_ *exec) {
  /* Cancel any pending timers */
  if (exec->timeout_timer) {
    xEventLoopTimerCancel(exec->loop, exec->timeout_timer);
    exec->timeout_timer = NULL;
  }
  if (exec->cancel_timer) {
    xEventLoopTimerCancel(exec->loop, exec->cancel_timer);
    exec->cancel_timer = NULL;
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

  /* Close PTY master fd on completion */
  if (exec->pty_master_fd >= 0) {
    exec->result.pty_fd = -1;
  }

  /* Unregister from SIGCHLD list */
  sigchld_remove(exec);
  sigchld_unregister(exec->loop);

  /* Clean up fds and event sources */
  cmd_cleanup(exec);

  /* Transition to idle BEFORE callback so xCommandExecutorSubmit can be called
   * again */
  exec->state = xCommandExecutorState_Idle;

  /* Deliver result — copy because callback may start a new run */
  xCommandResult result_copy = exec->result;
  exec->on_done((xCommandExecutor)exec, &result_copy, exec->ud);
}

/* ───────────────────── Cleanup ───────────────────── */

static void cmd_cleanup(struct xCommandExecutor_ *exec) {
  /* Remove event sources */
  if (exec->stdout_src) {
    xEventDel(exec->loop, exec->stdout_src);
    exec->stdout_src = NULL;
  }
  if (exec->stderr_src) {
    xEventDel(exec->loop, exec->stderr_src);
    exec->stderr_src = NULL;
  }

  /* Close stdin pipe (both ends; parent owns write end, read end
   * should already be closed after fork but be safe) */
  if (exec->stdin_pipe[0] >= 0) {
    close(exec->stdin_pipe[0]);
    exec->stdin_pipe[0] = -1;
  }
  if (exec->stdin_pipe[1] >= 0) {
    close(exec->stdin_pipe[1]);
    exec->stdin_pipe[1] = -1;
  }

  /* Close pipe read ends */
  if (exec->stdout_pipe[0] >= 0) {
    close(exec->stdout_pipe[0]);
    exec->stdout_pipe[0] = -1;
  }
  if (exec->stderr_pipe[0] >= 0) {
    close(exec->stderr_pipe[0]);
    exec->stderr_pipe[0] = -1;
  }

  /* Close pipe write ends (should already be closed, but be safe) */
  if (exec->stdout_pipe[1] >= 0) {
    close(exec->stdout_pipe[1]);
    exec->stdout_pipe[1] = -1;
  }
  if (exec->stderr_pipe[1] >= 0) {
    close(exec->stderr_pipe[1]);
    exec->stderr_pipe[1] = -1;
  }

  /* Close PTY master fd */
  if (exec->pty_master_fd >= 0) {
    close(exec->pty_master_fd);
    exec->pty_master_fd = -1;
  }
}

/* ───────────────────── Kill process group ───────────────────── */

static void cmd_kill_pg(struct xCommandExecutor_ *exec, int sig) {
  if (exec->child_pid > 0) {
    /* Kill the entire process group (negative pid = killpg) */
    kill(-exec->child_pid, sig);
  }
}
