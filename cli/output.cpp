/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * output.cpp - Output helpers.
 *
 * All AI-driven output must go through xLinePrintAbove / Chunk so the
 * user's prompt row stays intact while the model streams. The editor
 * is kept alive for the entire lifetime of the REPL (including during
 * AI runs), which is what lets the user type slash commands — notably
 * /cancel — while the model is thinking or streaming.
 *
 * above_printf() formats into a stack buffer then delegates to
 * xLinePrintAbove (which appends a trailing '\n' if missing) so the
 * call site reads like a plain std::printf. For long-running text
 * streams we use xLinePrintAboveChunk directly to preserve the
 * "token-by-token" visual cadence without forcing a newline.
 */

#include "output.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void above_printf(xLineHandle h, const char *fmt, ...) {
  if (!h) return;
  char    buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  xLinePrintAbove(h, buf);
}

void above_chunk(xLineHandle h, const char *s, size_t len) {
  if (!h || !s || len == 0) return;
  /* xLinePrintAboveChunk requires a NUL-terminated C string. Copy into
   * a stack buffer when the fragment fits; fall back to heap for the
   * rare oversized token. */
  char  stack[512];
  char *p = stack;
  if (len + 1 > sizeof(stack)) {
    p = (char *)std::malloc(len + 1);
    if (!p) return;
  }
  std::memcpy(p, s, len);
  p[len] = '\0';
  xLinePrintAboveChunk(h, p);
  if (p != stack) std::free(p);
}

/* Close an open thinking block: reset SGR (`\x1b[0m`) + a blank row so
 * whatever follows (final text, [tool], [done], ...) has visual
 * breathing room. The trailing blank is important when thinking ends
 * with a sentence that looks like a reply ("简短回复：..." etc) —
 * without it the eye merges the faint scratchpad into the bright
 * answer. Must be called before printing anything that shouldn't
 * inherit faint style. No-op if no thinking block is open, so it's
 * safe to sprinkle liberally. */
void end_thinking(ReplCtx *ctx) {
  if (!ctx->in_thinking) return;
  /* Two newlines: one to finish the trailing line, one for the blank
   * separator row. xLinePrintAbove will add another trailing '\n' if
   * the argument doesn't already end with one (it does, so no-op). */
  xLinePrintAbove(ctx->line, "\x1b[0m\n");
  ctx->in_thinking = false;
}

/* ── Renderer vtable implementations ────────────────────────────────
 *
 * Two backends: md (markdown → ANSI via xMd) and raw (verbatim
 * pass-through via above_chunk). Each is a const vtable + a void*
 * state pointer. The state for the md backend is &ctx->md_renderer;
 * the state for the raw backend is &ctx (it needs ctx->line to call
 * above_chunk). Switching is done by assigning ctx->renderer. */

static void renderer_md_feed(void *state, const char *data, size_t len) {
  xMdFeed(static_cast<xMd *>(state), data, len);
}

static void renderer_md_flush(void *state) {
  xMdFlush(static_cast<xMd *>(state));
}

static void renderer_md_reset(void *state) {
  xMdReset(static_cast<xMd *>(state));
}

static void renderer_raw_feed(void *state, const char *data, size_t len) {
  auto *ctx = static_cast<ReplCtx *>(state);
  above_chunk(ctx->line, data, len);
}

static void renderer_raw_flush(void *state) {
  (void)state; /* raw has no pending state to flush */
}

static void renderer_raw_reset(void *state) {
  (void)state; /* raw has no SGR spans to close */
}

const Renderer g_renderer_md  = {renderer_md_feed, renderer_md_flush, renderer_md_reset, nullptr};
const Renderer g_renderer_raw = {renderer_raw_feed, renderer_raw_flush, renderer_raw_reset,
                                 nullptr};

void renderer_use_md(ReplCtx *ctx) {
  ctx->renderer       = g_renderer_md;
  ctx->renderer.state = &ctx->md_renderer;
  ctx->renderer_name  = "md";
}

void renderer_use_raw(ReplCtx *ctx) {
  ctx->renderer       = g_renderer_raw;
  ctx->renderer.state = ctx;
  ctx->renderer_name  = "raw";
}
