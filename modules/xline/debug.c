/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * debug.c - Diagnostic logging
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"

/* ── debug_msg Compile-time knobs: -DIC_NO_DEBUG_MSG     strip all output
 * (debug.h stubs debug_msg) -DIC_DEBUG_TO_FILE    route into
 * ./isocline.debug.txt ── */

#if defined(IC_NO_DEBUG_MSG)
// nothing — debug.h turned debug_msg into a no-op macro.
#elif !defined(IC_DEBUG_TO_FILE)
ic_private void debug_msg(const char *fmt, ...) {
  if (getenv("ISOCLINE_DEBUG")) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
  }
}
#else
ic_private void debug_msg(const char *fmt, ...) {
  static int         debug_init;
  static const char *debug_fname = "isocline.debug.txt";
  // initialize?
  if (debug_init == 0) {
    debug_init         = -1;
    const char *rdebug = getenv("ISOCLINE_DEBUG");
    if (rdebug != NULL && strcmp(rdebug, "1") == 0) {
      FILE *fdbg = fopen(debug_fname, "w");
      if (fdbg != NULL) {
        debug_init = 1;
        fclose(fdbg);
      }
    }
  }
  if (debug_init <= 0) return;

  // write debug messages
  FILE *fdbg = fopen(debug_fname, "a");
  if (fdbg == NULL) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(fdbg, fmt, args);
  fclose(fdbg);
  va_end(args);
}
#endif
