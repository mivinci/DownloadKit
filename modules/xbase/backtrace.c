/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * backtrace.c - Platform-adaptive stack backtrace implementation
 *
 * Backend priority (selected at build time via CMake):
 *   1. libunwind  (XK_HAS_LIBUNWIND)
 *   2. execinfo   (XK_HAS_EXECINFO)  — macOS / Linux glibc
 *   3. stub       (fallback)
 */

#include <xbase/backtrace.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

/* Maximum number of frames to capture */
#define MAX_FRAMES 128

/*
 * Internal skip count: xBacktrace -> xBacktraceSkip -> bt_capture.
 * We skip 2 internal frames so the output starts at the caller of
 * xBacktrace / xBacktraceSkip.
 */
#define INTERNAL_SKIP 2

/* ═══════════════════════════════════════════════════════════════════
 * Backend 1: libunwind
 * ═══════════════════════════════════════════════════════════════════ */
#if defined(XK_HAS_LIBUNWIND)

#define UNW_LOCAL_ONLY
#include <libunwind.h>

static int bt_capture(int skip, char *buf, size_t size) {
  unw_cursor_t  cursor;
  unw_context_t ctx;
  unw_word_t    ip, off;
  char          sym[256];
  int           n          = 0; /* written bytes */
  int           frame      = 0;
  int           total_skip = skip + INTERNAL_SKIP;

  if (unw_getcontext(&ctx) < 0) return 0;
  if (unw_init_local(&cursor, &ctx) < 0) return 0;

  while (unw_step(&cursor) > 0) {
    if (total_skip > 0) {
      total_skip--;
      continue;
    }

    unw_get_reg(&cursor, UNW_REG_IP, &ip);

    int ret;
    if (unw_get_proc_name(&cursor, sym, sizeof(sym), &off) == 0) {
      ret = snprintf(buf + n, size - (size_t)n, "#%d 0x%lx %s+0x%lx\n", frame,
                     (unsigned long)ip, sym, (unsigned long)off);
    } else {
      ret = snprintf(buf + n, size - (size_t)n, "#%d 0x%lx <unknown>\n", frame,
                     (unsigned long)ip);
    }

    if (ret < 0) break;

    if ((size_t)(n + ret) >= size) {
      /* Truncated — buf is already NUL-terminated by snprintf */
      n = (int)(size - 1);
      break;
    }

    n += ret;
    frame++;
  }

  return n;
}

/* ═══════════════════════════════════════════════════════════════════
 * Backend 2: execinfo (macOS / Linux glibc)
 *
 * NOTE: Link with -rdynamic on Linux for meaningful symbol names.
 * ═══════════════════════════════════════════════════════════════════ */
#elif defined(XK_HAS_EXECINFO)

#include <execinfo.h>
#include <stdlib.h>

static int bt_capture(int skip, char *buf, size_t size) {
  void  *frames[MAX_FRAMES];
  int    depth;
  char **syms;
  int    n          = 0;
  int    frame      = 0;
  int    total_skip = skip + INTERNAL_SKIP;

  depth = backtrace(frames, MAX_FRAMES);
  if (depth <= 0) return 0;

  syms = backtrace_symbols(frames, depth);

  for (int i = 0; i < depth; i++) {
    if (total_skip > 0) {
      total_skip--;
      continue;
    }

    int ret;
    if (syms) {
      ret = snprintf(buf + n, size - (size_t)n, "#%d %s\n", frame, syms[i]);
    } else {
      ret = snprintf(buf + n, size - (size_t)n, "#%d %p <unknown>\n", frame,
                     frames[i]);
    }

    if (ret < 0) break;

    if ((size_t)(n + ret) >= size) {
      n = (int)(size - 1);
      break;
    }

    n += ret;
    frame++;
  }

  free(syms);
  return n;
}

/* ═══════════════════════════════════════════════════════════════════
 * Backend 3: stub (unsupported platform)
 * ═══════════════════════════════════════════════════════════════════ */
#else

static int bt_capture(int skip, char *buf, size_t size) {
  (void)skip;
  static const char msg[] = "<backtrace not supported on this platform>\n";
  size_t            len   = sizeof(msg) - 1; /* exclude NUL */

  if (len >= size) len = size - 1;

  memcpy(buf, msg, len);
  buf[len] = '\0';
  return (int)len;
}

#endif /* backend selection */

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

int xBacktraceSkip(int skip, char *buf, size_t size) {
  if (!buf || size == 0) return 0;

  buf[0] = '\0';
  return bt_capture(skip, buf, size);
}

int xBacktrace(char *buf, size_t size) {
  return xBacktraceSkip(0, buf, size);
}

/* ═══════════════════════════════════════════════════════════════════
 * Crash handler — print backtrace on fatal signals
 * ═══════════════════════════════════════════════════════════════════ */

static void xbt_crash_handler(int sig) {
  const char *name = "UNKNOWN";
  switch (sig) {
#ifdef SIGSEGV
    case SIGSEGV: name = "SIGSEGV"; break;
#endif
#ifdef SIGABRT
    case SIGABRT: name = "SIGABRT"; break;
#endif
#ifdef SIGBUS
    case SIGBUS:  name = "SIGBUS";  break;
#endif
    default: break;
  }

  char buf[4096];
  int  n;

  fprintf(stderr, "\n=== CRASH: signal %s (%d) ===\n", name, sig);

  n = bt_capture(1, buf, sizeof(buf)); /* skip this handler frame */
  if (n > 0) {
    fprintf(stderr, "Backtrace:\n%s", buf);
  }

  fprintf(stderr, "===========================\n");

  /* Re-raise with default handler so the OS can produce a core dump */
  signal(sig, SIG_DFL);
  raise(sig);
}

void xPrintBacktraceOnCrash(void) {
#ifdef SIGSEGV
  signal(SIGSEGV, xbt_crash_handler);
#endif
#ifdef SIGABRT
  signal(SIGABRT, xbt_crash_handler);
#endif
#ifdef SIGBUS
  signal(SIGBUS,  xbt_crash_handler);
#endif
}
