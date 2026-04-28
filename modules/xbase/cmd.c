/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cmd.c - Async command executor over xEventLoop
 */

#include <xbase/cmd.h>

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
#include <util.h>    /* forkpty on macOS */
#elif defined(XK_HAS_PTY_H)
#include <pty.h>     /* forkpty on Linux */
#endif
#endif

/* ───────────────────── Constants ───────────────────── */

#define CMD_CANCEL_GRACE_MS 5000 /**< SIGTERM → SIGKILL grace period */
#define CMD_READ_BUF_SIZE   4096 /**< Per-read buffer size           */

/* ───────────────────── Local auto-growing buffer ───────────────────── */

/**
 * Lightweight auto-growing byte buffer for capturing command output.
 * Avoids a circular dependency between xbase and xbuf.
 */
typedef struct CmdBuf {
  char  *data;   /**< Allocated data (always NUL-terminated after finalize) */
  size_t len;    /**< Number of valid bytes (not counting NUL terminator)   */
  size_t cap;    /**< Allocated capacity                                     */
} CmdBuf;

static CmdBuf *cmdbuf_create(size_t initial_cap) {
  CmdBuf *b = (CmdBuf *)malloc(sizeof(CmdBuf));
  if (!b) return NULL;
  if (initial_cap < 64) initial_cap = 64;
  b->data = (char *)malloc(initial_cap);
  if (!b->data) { free(b); return NULL; }
  b->len = 0;
  b->cap = initial_cap;
  b->data[0] = '\0';
  return b;
}

static void cmdbuf_destroy(CmdBuf *b) {
  if (!b) return;
  free(b->data);
  free(b);
}

static int cmdbuf_append(CmdBuf *b, const char *src, size_t n) {
  if (!b || n == 0) return 0;
  if (b->len + n + 1 > b->cap) {
    size_t new_cap = b->cap;
    while (new_cap < b->len + n + 1) new_cap *= 2;
    char *tmp = (char *)realloc(b->data, new_cap);
    if (!tmp) return -1;
    b->data = tmp;
    b->cap  = new_cap;
  }
  memcpy(b->data + b->len, src, n);
  b->len += n;
  b->data[b->len] = '\0'; /* Keep NUL-terminated */
  return 0;
}

/* ───────────────────── Internal state ───────────────────── */

enum xCommandState_ {
  xCommandState_Idle = 0,
  xCommandState_Running,
  xCommandState_Cancelling, /**< SIGTERM sent, waiting for exit or SIGKILL */
};

struct xCommand_ {
  xEventLoop loop;

  /* Child process */
  pid_t         child_pid;
  enum xCommandState_ state;

  /* Pipes: [0] = read end (parent), [1] = write end (child) */
  int stdout_pipe[2];
  int stderr_pipe[2];

  /* PTY master fd (valid in PTY mode, -1 otherwise) */
  int pty_master_fd;

  /* Event sources for pipe read ends / PTY master */
  xEventSource stdout_src;
  xEventSource stderr_src;

  /* Timeout timer */
  xEventTimer timeout_timer;

  /* Cancel grace timer (SIGTERM → SIGKILL) */
  xEventTimer cancel_timer;

  /* Output capture buffers (Capture mode) */
  CmdBuf *stdout_buf;
  CmdBuf *stderr_buf;

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
  xCommandOutputFunc on_stdout;
  xCommandOutputFunc on_stderr;
  xCommandDoneFunc   on_done;
  void          *ud;

  /* Timing */
  uint64_t start_ms;

  /* Track which pipes are still open */
  int stdout_eof;
  int stderr_eof;
  int child_exited;

  /* Linked list for SIGCHLD multiplexer */
  struct xCommand_ *next;
};

/* ───────────────────── SIGCHLD multiplexer ───────────────────── */

/*
 * Multiple xCommand instances may be active on the same event loop.
 * We maintain a singly-linked list of running executors and
 * register SIGCHLD once. When SIGCHLD fires we walk the list
 * and call waitpid(WNOHANG) for each.
 */

static struct xCommand_ *g_sigchld_head = NULL;
static int           g_sigchld_count = 0;

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

static void sigchld_add(struct xCommand_ *exec) {
  exec->next = g_sigchld_head;
  g_sigchld_head = exec;
}

static void sigchld_remove(struct xCommand_ *exec) {
  struct xCommand_ **pp = &g_sigchld_head;
  while (*pp) {
    if (*pp == exec) {
      *pp = exec->next;
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
static void cmd_check_completion(struct xCommand_ *exec);
static void cmd_fire_done(struct xCommand_ *exec);
static void cmd_cleanup(struct xCommand_ *exec);
static void cmd_kill_pg(struct xCommand_ *exec, int sig);
#ifdef XK_HAS_PTY
static xErrno xCommandRunPty(struct xCommand_ *exec, const xCommandConf *conf);
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

xCommand xCommandCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xCommand_ *exec = (struct xCommand_ *)calloc(1, sizeof(*exec));
  if (!exec) return NULL;

  exec->loop           = loop;
  exec->child_pid      = -1;
  exec->stdout_pipe[0] = -1;
  exec->stdout_pipe[1] = -1;
  exec->stderr_pipe[0] = -1;
  exec->stderr_pipe[1] = -1;
  exec->pty_master_fd  = -1;
  exec->stdout_src     = NULL;
  exec->stderr_src     = NULL;
  exec->timeout_timer  = NULL;
  exec->cancel_timer   = NULL;
  exec->state          = xCommandState_Idle;
  exec->stdout_buf     = NULL;
  exec->stderr_buf     = NULL;

  return (xCommand)exec;
}

void xCommandDestroy(xCommand exec_) {
  if (!exec_) return;
  struct xCommand_ *exec = (struct xCommand_ *)exec_;

  if (exec->state != xCommandState_Idle) {
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

  if (exec->stdout_buf) cmdbuf_destroy(exec->stdout_buf);
  if (exec->stderr_buf) cmdbuf_destroy(exec->stderr_buf);
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
    while (user_argv[argc - 1]) argc++;
  }
  const char **exec_argv = (const char **)malloc((argc + 1) * sizeof(const char *));
  if (!exec_argv) return NULL;
  exec_argv[0] = cmd;
  if (user_argv) {
    for (int i = 1; i < argc; i++) exec_argv[i] = user_argv[i - 1];
  }
  exec_argv[argc] = NULL;
  return exec_argv;
}

/* ───────────────────── Execution ───────────────────── */

xErrno xCommandRun(xCommand exec_, const xCommandConf *conf,
                xCommandOutputFunc on_stdout, xCommandOutputFunc on_stderr,
                xCommandDoneFunc on_done, void *ud) {
  if (!exec_ || !conf || !conf->cmd || !on_done)
    return xErrno_InvalidArg;

  struct xCommand_ *exec = (struct xCommand_ *)exec_;

  if (exec->state != xCommandState_Idle)
    return xErrno_Busy;

  /* ── Reset state from previous run ── */
  if (exec->stdout_buf) cmdbuf_destroy(exec->stdout_buf);
  if (exec->stderr_buf) cmdbuf_destroy(exec->stderr_buf);
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
    exec->stdout_buf = cmdbuf_create(4096);
    if (!exec->stdout_buf) goto fail;
  }
  if (conf->stderr_mode == xCommandOutput_Capture &&
      conf->input_mode != xCommandInput_Pty) {
    exec->stderr_buf = cmdbuf_create(4096);
    if (!exec->stderr_buf) goto fail;
  }
  exec->stdout_eof  = 0;
  exec->stderr_eof  = 0;
  exec->child_exited = 0;
  exec->pty_master_fd = -1;
  exec->result.pty_fd = -1;

  exec->on_stdout  = on_stdout;
  exec->on_stderr  = on_stderr;
  exec->on_done    = on_done;
  exec->ud         = ud;

  /* ── PTY mode ── */
  if (conf->input_mode == xCommandInput_Pty) {
#ifdef XK_HAS_PTY
    return xCommandRunPty(exec, conf);
#else
    return xErrno_NotSupported;
#endif
  }

  /* ── Pipe mode (default) ── */

  /* ── Create pipes ── */
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
  pid_t pid = fork();
  if (pid < 0) {
    sigchld_remove(exec);
    sigchld_unregister(exec->loop);
    goto fail_pipes;
  }

  if (pid == 0) {
    /* ── Child process ── */

    /* Create own process group for killpg() support */
    setpgid(0, 0);

    /* Redirect stdout */
    if (conf->stdout_mode == xCommandOutput_Discard) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
    } else {
      dup2(child_stdout_wfd, STDOUT_FILENO);
    }

    /* Redirect stderr */
    if (conf->stderr_mode == xCommandOutput_Discard) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    } else {
      dup2(child_stderr_wfd, STDERR_FILENO);
    }

    /* Close all pipe fds (write ends are now dup'd to stdout/stderr) */
    close(exec->stdout_pipe[0]);
    close(exec->stdout_pipe[1]);
    close(exec->stderr_pipe[0]);
    close(exec->stderr_pipe[1]);

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

  /* Synchronize process group creation to avoid race with child's setpgid(0,0). */
  setpgid(pid, pid);

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
  if (conf->stdout_mode != xCommandOutput_Discard && exec->stdout_pipe[0] >= 0) {
    exec->stdout_src = xEventAdd(exec->loop, exec->stdout_pipe[0],
                                  xEvent_Read, on_stdout_readable, exec);
    if (!exec->stdout_src) goto fail_sigchld;
  } else {
    exec->stdout_eof = 1;
  }

  if (conf->stderr_mode != xCommandOutput_Discard && exec->stderr_pipe[0] >= 0) {
    exec->stderr_src = xEventAdd(exec->loop, exec->stderr_pipe[0],
                                  xEvent_Read, on_stderr_readable, exec);
    if (!exec->stderr_src) goto fail_sigchld;
  } else {
    exec->stderr_eof = 1;
  }

  /* Start timeout timer */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer = xEventLoopTimerAfter(exec->loop, on_timeout, exec,
                                                conf->timeout_ms);
  }

  exec->state = xCommandState_Running;
  return xErrno_Ok;

fail_sigchld:
  sigchld_remove(exec);
  sigchld_unregister(exec->loop);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
fail_pipes:
  if (exec->stdout_pipe[0] >= 0) close(exec->stdout_pipe[0]);
  if (exec->stdout_pipe[1] >= 0) close(exec->stdout_pipe[1]);
  if (exec->stderr_pipe[0] >= 0) close(exec->stderr_pipe[0]);
  if (exec->stderr_pipe[1] >= 0) close(exec->stderr_pipe[1]);
  exec->stdout_pipe[0] = -1;
  exec->stdout_pipe[1] = -1;
  exec->stderr_pipe[0] = -1;
  exec->stderr_pipe[1] = -1;
fail:
  exec->state = xCommandState_Idle;
  return xErrno_SysError;
}

/* ───────────────────── PTY mode execution ───────────────────── */

#ifdef XK_HAS_PTY

static xErrno xCommandRunPty(struct xCommand_ *exec, const xCommandConf *conf) {
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

  int master_fd = -1;
  pid_t pid;

#if defined(__APPLE__) || defined(__linux__)
  /* forkpty() is available on both macOS and Linux */
  pid = forkpty(&master_fd, NULL, NULL, NULL);
#else
  /* Fallback: use posix_openpt() + fork() */
  /* This path is for other POSIX systems; currently not needed. */
  pid = -1;
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

    /* Create own process group for killpg() support.
     * Note: forkpty already sets up the slave as controlling terminal,
     * but we still want our own process group for clean killpg(). */
    setpgid(0, 0);

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

  exec->child_pid = pid;
  exec->pty_master_fd = master_fd;
  exec->result.pty_fd = master_fd;

  /* Synchronize process group creation to avoid race with child's setpgid(0,0). */
  setpgid(pid, pid);

  /* Set master fd to non-blocking for event loop integration */
  if (fd_set_nonblock(master_fd) != 0) {
    goto fail_sigchld;
  }

  /* Probe: the child may have already exited */
  {
    int   status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    fprintf(stderr, "[DEBUG PTY probe] pid=%d ret=%d status=0x%x WIFEXITED=%d WEXITSTATUS=%d WIFSIGNALED=%d WTERMSIG=%d\n",
            pid, ret, status, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status), WTERMSIG(status));
    if (ret == pid) {
      exec->child_exited = 1;
      if (WIFEXITED(status)) {
        exec->result.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exec->result.signaled = WTERMSIG(status);
      }
    }
  }

  /* In PTY mode, we always mark stderr as EOF since it's merged */
  exec->stderr_eof = 1;

  /* Watch master fd for output (unless discarded) */
  if (conf->stdout_mode != xCommandOutput_Discard) {
    exec->stdout_src = xEventAdd(exec->loop, master_fd,
                                  xEvent_Read, on_pty_readable, exec);
    if (!exec->stdout_src) goto fail_sigchld;
  } else {
    /* Discard mode: close master fd immediately since we don't read it.
     * Actually, we should keep it open so the child doesn't get SIGPIPE
     * when writing to a closed PTY. Instead, just mark it as EOF for
     * our tracking but don't close the fd. */
    exec->stdout_eof = 1;
  }

  /* Start timeout timer */
  if (conf->timeout_ms > 0) {
    exec->timeout_timer = xEventLoopTimerAfter(exec->loop, on_timeout, exec,
                                                conf->timeout_ms);
  }

  exec->state = xCommandState_Running;
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
  exec->state = xCommandState_Idle;
  return xErrno_SysError;
}

#endif /* XK_HAS_PTY */

/* ───────────────────── Cancel ───────────────────── */

xErrno xCommandCancel(xCommand exec_) {
  if (!exec_) return xErrno_InvalidArg;
  struct xCommand_ *exec = (struct xCommand_ *)exec_;

  if (exec->state != xCommandState_Running) return xErrno_InvalidState;

  /* Send SIGTERM to the process group */
  cmd_kill_pg(exec, SIGTERM);
  exec->result.timed_out = 1;
  exec->state = xCommandState_Cancelling;

  /* Start grace timer for SIGKILL */
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                             CMD_CANCEL_GRACE_MS);
  return xErrno_Ok;
}

/* ───────────────────── Query ───────────────────── */

int xCommandPid(xCommand exec_) {
  if (!exec_) return -1;
  struct xCommand_ *exec = (struct xCommand_ *)exec_;
  return (exec->state != xCommandState_Idle) ? (int)exec->child_pid : -1;
}

int xCommandIsRunning(xCommand exec_) {
  if (!exec_) return 0;
  return ((struct xCommand_ *)exec_)->state != xCommandState_Idle;
}

int xCommandPtyFd(xCommand exec_) {
  if (!exec_) return -1;
  struct xCommand_ *exec = (struct xCommand_ *)exec_;
  if (exec->state == xCommandState_Idle) return -1;
  return exec->pty_master_fd;
}

/* ───────────────────── Pipe read callbacks ───────────────────── */

static void on_stdout_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommand_ *exec = (struct xCommand_ *)arg;
  char buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (exec->stdout_mode == xCommandOutput_Capture) {
        cmdbuf_append(exec->stdout_buf, buf, (size_t)n);
      } else if (exec->stdout_mode == xCommandOutput_Stream && exec->on_stdout) {
        exec->on_stdout((xCommand)exec, buf, (size_t)n, exec->ud);
      }
    } else if (n == 0) {
      /* EOF */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      close(exec->stdout_pipe[0]);
      exec->stdout_pipe[0] = -1;
      exec->stdout_eof = 1;
      cmd_check_completion(exec);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      /* Error — treat as EOF */
      xEventDel(exec->loop, exec->stdout_src);
      exec->stdout_src = NULL;
      close(exec->stdout_pipe[0]);
      exec->stdout_pipe[0] = -1;
      exec->stdout_eof = 1;
      cmd_check_completion(exec);
      break;
    }
  }
}

static void on_stderr_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommand_ *exec = (struct xCommand_ *)arg;
  char buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (exec->stderr_mode == xCommandOutput_Capture) {
        cmdbuf_append(exec->stderr_buf, buf, (size_t)n);
      } else if (exec->stderr_mode == xCommandOutput_Stream && exec->on_stderr) {
        exec->on_stderr((xCommand)exec, buf, (size_t)n, exec->ud);
      }
    } else if (n == 0) {
      xEventDel(exec->loop, exec->stderr_src);
      exec->stderr_src = NULL;
      close(exec->stderr_pipe[0]);
      exec->stderr_pipe[0] = -1;
      exec->stderr_eof = 1;
      cmd_check_completion(exec);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      xEventDel(exec->loop, exec->stderr_src);
      exec->stderr_src = NULL;
      close(exec->stderr_pipe[0]);
      exec->stderr_pipe[0] = -1;
      exec->stderr_eof = 1;
      cmd_check_completion(exec);
      break;
    }
  }
}

/* ───────────────────── PTY read callback ───────────────────── */

#ifdef XK_HAS_PTY

static void on_pty_readable(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xCommand_ *exec = (struct xCommand_ *)arg;
  char buf[CMD_READ_BUF_SIZE];

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      /* In PTY mode, all output is merged and treated as stdout */
      if (exec->stdout_mode == xCommandOutput_Capture) {
        cmdbuf_append(exec->stdout_buf, buf, (size_t)n);
      } else if (exec->stdout_mode == xCommandOutput_Stream && exec->on_stdout) {
        exec->on_stdout((xCommand)exec, buf, (size_t)n, exec->ud);
      }
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
      /* EIO is commonly returned when the slave side is closed (child exited) */
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
  struct xCommand_ *exec = (struct xCommand_ *)arg;
  exec->timeout_timer = NULL;

  if (exec->state != xCommandState_Running) return;

  /* Send SIGTERM, then schedule SIGKILL */
  cmd_kill_pg(exec, SIGTERM);
  exec->result.timed_out = 1;
  exec->state = xCommandState_Cancelling;
  exec->cancel_timer = xEventLoopTimerAfter(exec->loop, on_cancel_grace, exec,
                                             CMD_CANCEL_GRACE_MS);
}

/* ───────────────────── Cancel grace period ───────────────────── */

static void on_cancel_grace(void *arg) {
  struct xCommand_ *exec = (struct xCommand_ *)arg;
  exec->cancel_timer = NULL;

  if (exec->state != xCommandState_Cancelling) return;

  /* Child didn't exit after SIGTERM — force kill */
  cmd_kill_pg(exec, SIGKILL);
  /* SIGCHLD handler will reap and complete */
}

/* ───────────────────── SIGCHLD handler ───────────────────── */

static void sigchld_handler(int signo, void *arg) {
  (void)signo;
  (void)arg;

  /* Walk all registered executors and try to reap. */
  struct xCommand_ *cur = g_sigchld_head;
  while (cur) {
    struct xCommand_ *exec = cur;
    if (exec->child_pid > 0 && !exec->child_exited) {
      int   status;
      pid_t ret = waitpid(exec->child_pid, &status, WNOHANG);
      fprintf(stderr, "[DEBUG SIGCHLD] pid=%d ret=%d status=0x%x WIFEXITED=%d WEXITSTATUS=%d WIFSIGNALED=%d errno=%d\n",
              exec->child_pid, ret, status, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status), errno);
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
        fprintf(stderr, "[DEBUG SIGCHLD ECHILD] pid=%d exit_code already=%d, setting child_exited=1\n",
                exec->child_pid, exec->result.exit_code);
        exec->child_exited = 1;
        cmd_check_completion(exec);
      }
    }
    cur = exec->next;
  }
}

/* ───────────────────── Completion check ───────────────────── */

static void cmd_check_completion(struct xCommand_ *exec) {
  /* All three conditions must be met: stdout EOF, stderr EOF, child exited */
  if (!exec->stdout_eof || !exec->stderr_eof || !exec->child_exited) return;

  cmd_fire_done(exec);
}

/* ───────────────────── Fire completion ───────────────────── */

static void cmd_fire_done(struct xCommand_ *exec) {
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
    exec->result.stdout_buf = exec->stdout_buf->data;
    exec->result.stdout_len = exec->stdout_buf->len;
  }
  if (exec->stderr_mode == xCommandOutput_Capture && exec->input_mode != xCommandInput_Pty
      && exec->stderr_buf) {
    exec->result.stderr_buf = exec->stderr_buf->data;
    exec->result.stderr_len = exec->stderr_buf->len;
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

  /* Transition to idle BEFORE callback so xCommandRun can be called again */
  exec->state = xCommandState_Idle;

  /* Deliver result — copy because callback may start a new run */
  xCommandResult result_copy = exec->result;
  exec->on_done((xCommand)exec, &result_copy, exec->ud);
}

/* ───────────────────── Cleanup ───────────────────── */

static void cmd_cleanup(struct xCommand_ *exec) {
  /* Remove event sources */
  if (exec->stdout_src) {
    xEventDel(exec->loop, exec->stdout_src);
    exec->stdout_src = NULL;
  }
  if (exec->stderr_src) {
    xEventDel(exec->loop, exec->stderr_src);
    exec->stderr_src = NULL;
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

static void cmd_kill_pg(struct xCommand_ *exec, int sig) {
  if (exec->child_pid > 0) {
    /* Kill the entire process group (negative pid = killpg) */
    kill(-exec->child_pid, sig);
  }
}
