/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xline_async.c - Drive xline's non-blocking line editor from a
 *                 xbase event loop.
 *
 * The demo shows the minimal glue needed for an AI TUI: a single
 * xEventLoop multiplexes the tty fd returned by xLineFd() together
 * with a periodic timer that plays the role of an "AI token stream".
 * Tokens arrive on the loop thread and are drawn above the prompt
 * via xLinePrintAbove(), so the user's in-progress input is never
 * clobbered.
 *
 * Commands:
 *   /quit   - leave the demo
 *   /start  - begin emitting fake AI tokens (every 200ms)
 *   /stop   - stop the token stream
 *   anything else is echoed back via xLinePrintAbove().
 *
 * Ctrl-C (SIGINT) asks the event loop to stop cleanly.
 *
 * POSIX only. On Windows xLineFd() returns -1 and this demo refuses
 * to run; a Win32 port would block on xLineStep() inside a worker
 * thread and bounce events back with xEventLoopPost().
 */

#include <xbase/event.h>
#include <xline/line.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── demo state ─────────────────────────────────────────────────────── */
typedef struct Demo {
  xEventLoop   loop;
  xLineHandle  line; /* current async editor session, or NULL        */
  xEventSource src;  /* loop registration for xLineFd(), or NULL     */
  xEventTimer  tick; /* fake AI token timer, or NULL when idle       */
  int          ntok; /* tokens emitted in the current "reply"        */
  bool         quit; /* set when user typed /quit or hit EOF         */
} Demo;

/* ── line session lifecycle ─────────────────────────────────────────── */
static void demo_line_cb(int fd, xEventMask mask, void *arg);

static int demo_open_line(Demo *d) {
  d->line = xLineBegin("ai> ");
  if (!d->line) {
    fprintf(stderr, "xLineBegin failed (dumb tty? another session live?)\n");
    return -1;
  }
  int fd = xLineFd(d->line);
  if (fd < 0) {
    fprintf(stderr, "xLineFd returned %d — not pollable on this platform\n",
            fd);
    xLineEnd(d->line);
    d->line = NULL;
    return -1;
  }
  d->src = xEventAdd(d->loop, fd, xEvent_Read, demo_line_cb, d);
  if (!d->src) {
    fprintf(stderr, "xEventAdd failed for tty fd=%d\n", fd);
    xLineEnd(d->line);
    d->line = NULL;
    return -1;
  }
  return 0;
}

static void demo_close_line(Demo *d) {
  if (d->src) {
    xEventDel(d->loop, d->src);
    d->src = NULL;
  }
  if (d->line) {
    xLineEnd(d->line);
    d->line = NULL;
  }
}

/* ── fake AI token stream ───────────────────────────────────────────── */
static const char *kFakeTokens[] = {
  "Sure, ",   "here ", "is ",    "a ",  "streamed ", "reply ", "that ",
  "arrives ", "one ",  "token ", "at ", "a ",        "time.",
};
#define FAKE_TOKEN_COUNT ((int)(sizeof(kFakeTokens) / sizeof(kFakeTokens[0])))

static void demo_tick_cb(void *arg);

static void demo_start_stream(Demo *d) {
  if (d->tick) return; /* already running */
  d->ntok = 0;
  d->tick = xEventLoopTimerAfter(d->loop, demo_tick_cb, d, 1000);
  if (!d->tick) fprintf(stderr, "failed to arm fake-AI timer\n");
}

static void demo_stop_stream(Demo *d) {
  if (!d->tick) return;
  xEventLoopTimerCancel(d->loop, d->tick);
  d->tick = NULL;
}

static void demo_tick_cb(void *arg) {
  Demo *d = (Demo *)arg;
  d->tick = NULL; /* builtin timer is one-shot */
  if (!d->line) return;

  if (d->ntok < FAKE_TOKEN_COUNT) {
    /* Stream each token on the same line above the prompt. */
    xLinePrintAboveChunk(d->line, kFakeTokens[d->ntok++]);
    /* Re-arm for the next token. */
    d->tick = xEventLoopTimerAfter(d->loop, demo_tick_cb, d, 500);
  } else {
    /* Close the streaming region with a status line; this also repaints
     * the prompt cleanly on a fresh row. */
    xLinePrintAbove(d->line, "\n[stream done]");
  }
}

/* ── user input handling ────────────────────────────────────────────── */
static void demo_handle_line(Demo *d, char *line) {
  if (!line) return;

  if (strcmp(line, "/quit") == 0) {
    d->quit = true;
  } else if (strcmp(line, "/start") == 0) {
    /* The line session was torn down before we got here, so the editor
     * is not live; just print to stdout in cooked mode. */
    fputs("[starting fake stream]\n", stdout);
    fflush(stdout);
    demo_start_stream(d);
  } else if (strcmp(line, "/stop") == 0) {
    demo_stop_stream(d);
    fputs("[stream stopped]\n", stdout);
    fflush(stdout);
  } else {
    char buf[512];
    snprintf(buf, sizeof buf, "[echo] %s\n", line);
    /* The session that produced `line` has just been closed below,
     * so print to stdout directly in that window. We'll reopen a
     * fresh session right after. */
    fputs(buf, stdout);
    fflush(stdout);
    xLineHistoryAdd(line);
  }
}

static void demo_line_cb(int fd, xEventMask mask, void *arg) {
  (void)fd;
  (void)mask;
  Demo *d = (Demo *)arg;
  if (!d->line) return;

  for (;;) {
    xLineStepResult r = xLineStep(d->line);
    switch (r) {
    case XLINE_STEP_PENDING:
      return; /* wait for more bytes */
    case XLINE_STEP_LINE: {
      char *s = xLineTake(d->line);
      /* Tear down the current session before handling the line, so
       * any stdout writes during handling happen in cooked mode and
       * don't fight with the editor. */
      demo_close_line(d);
      demo_handle_line(d, s);
      xLineFree(s);
      if (d->quit) {
        xEventLoopStop(d->loop);
        return;
      }
      if (demo_open_line(d) != 0) {
        xEventLoopStop(d->loop);
        return;
      }
      return;
    }
    case XLINE_STEP_EOF:
    case XLINE_STEP_ERROR:
      fputs(r == XLINE_STEP_EOF ? "\n[eof]\n" : "\n[error]\n", stdout);
      fflush(stdout);
      demo_close_line(d);
      xEventLoopStop(d->loop);
      return;
    case XLINE_STEP_INTERRUPT:
      /* ^C on a bare demo prompt: just treat it as "quit". */
      fputs("\n[interrupt]\n", stdout);
      fflush(stdout);
      demo_close_line(d);
      xEventLoopStop(d->loop);
      return;
    }
  }
}

/* ── signals ────────────────────────────────────────────────────────── */
static void demo_on_sigint(int signo, void *arg) {
  (void)signo;
  Demo *d = (Demo *)arg;
  /* Unblock the editor; xLineStep() will then return EOF which our
   * line callback turns into xEventLoopStop(). */
  xLineAsyncStop();
  (void)d;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
  Demo d = {0};

  d.loop = xEventLoopCreate();
  if (!d.loop) {
    fprintf(stderr, "xEventLoopCreate failed\n");
    return 1;
  }

  xLineSetHistory(NULL, 100);
  /* Suppress the default "> " marker so our "ai> " prompt is not
   * decorated into "ai> > ". */
  xLineSetPromptMarker("", NULL);
  xLineStyleDef("prompt", "bold #66ccff");

  /* Let the prompt flow naturally after any above-region output
   * rather than being anchored to the bottom row of the terminal. */
  xLineEnableAnchor(false);

  /* Print any banner text *before* opening the line session. xLineBegin
   * paints the prompt at the current cursor position, so anything we
   * puts/printf after it would appear on the prompt's row until the next
   * edit_refresh rewrites it. */
  puts("xline async demo — type /start, /stop, /quit, or any text.");
  fflush(stdout);

  if (demo_open_line(&d) != 0) {
    xEventLoopDestroy(d.loop);
    return 1;
  }

  xEventLoopSignalWatch(d.loop, SIGINT, demo_on_sigint, &d);

  xEventLoopRun(d.loop);

  demo_stop_stream(&d);
  demo_close_line(&d);
  xEventLoopSignalWatch(d.loop, SIGINT, NULL, NULL);
  xEventLoopDestroy(d.loop);
  return 0;
}
