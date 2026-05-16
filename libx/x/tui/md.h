/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md.h - Streaming markdown -> ANSI transformer (P0).
 *
 * The assistant stream is markdown by convention but the terminal
 * only speaks ANSI/VT. This translator sits between a byte stream
 * producer (e.g. xAgent on_text) and a byte sink (e.g. an xline
 * above-prompt writer): it consumes arbitrary byte-sliced chunks,
 * emits the same bytes with inline markdown structures rewritten as
 * SGR escapes, and carries a tiny pending buffer across chunk
 * boundaries so a `**` split into two deltas still becomes bold.
 *
 * Scope (P0 - intentionally small):
 *   - **bold**
 *   - *italic*, _italic_
 *   - `inline code` (rendered via reverse-video, not colour, to
 *     avoid clashing with surrounding theme/chrome SGR)
 *   - # / ## / ### heading lines (whole line bolded)
 *   - ```fenced code blocks``` are passed through untouched, so
 *     `for i in *` inside a shell block stays literal.
 *
 * Explicitly out of scope at P0: links, lists, tables, blockquotes,
 * strikethrough, html, nested emphasis. Those either need multi-line
 * lookahead (lists/tables) or a real inline parser (links); both can
 * be layered on top of this state machine without rewriting the
 * Feed/Flush contract.
 *
 * Contract:
 *   - Call xMdFeed() for every chunk. Output bytes are routed to
 *     the caller-supplied sink synchronously, 0+ times per call.
 *     The sink must be cheap.
 *   - Call xMdFlush() exactly once at end-of-stream. It drains
 *     pending delimiter candidates and emits an SGR reset so no
 *     style leaks into whatever renders next.
 *   - xMdReset() discards pending bytes and emits an SGR reset.
 *     Used on mid-stream cancel.
 *
 * Thread-safety: none. Designed for single-threaded event-loop use.
 */

#ifndef XTUI_MD_H
#define XTUI_MD_H

#include <stddef.h>

#include <x/base/base.h>

/**
 * @brief Byte sink invoked by the renderer. `data` is NOT
 *        NUL-terminated; always honour `len`.
 */
typedef void (*xMdSinkFunc)(const char *data, size_t len, void *arg);

/**
 * @brief Streaming markdown-to-ANSI renderer state.
 *
 * Fields are exposed only so callers can embed the struct by value
 * (zero heap allocation). Treat them as private - only touch them
 * through xMd* functions.
 *
 * Pending buffer holds at most 3 bytes at P0 (the longest prefix we
 * ever stash is "```", "***", or "###"). The UTF-8 pending buffer
 * also holds at most 3 bytes (the max continuation tail of a 4-byte
 * sequence minus the leading byte). Fixed inline arrays beat a heap
 * allocation for these tiny scratchpads.
 */
XDEF_STRUCT(xMd) {
  xMdSinkFunc sink;
  void       *arg;

  /* Open SGR spans. Bitfield because emphasis can nest (bold can
   * wrap italic in markdown) and the terminal handles the SGR
   * stack implicitly - we just track which close code to emit. */
  unsigned bold : 1;
  unsigned italic : 1;
  unsigned code : 1;    /* inline `..` */
  unsigned heading : 1; /* current line is a heading */
  unsigned fence : 1;   /* inside ```...``` - pass through raw */

  /* True when the next emitted byte starts a line. Seeded 1 so the
   * very first chunk starts in bol state. */
  unsigned bol : 1;

  /* Pending delimiter candidate bytes. */
  char   pending[4];
  size_t pending_n;

  /* UTF-8 continuation bytes that arrived at the end of a chunk
   * without their leading byte. Buffered until the next xMdFeed
   * call delivers the rest of the character. A UTF-8 sequence is
   * at most 4 bytes so 3 is the max we'd stash (1 leading + up to
   * 3 continuation, but if the leading byte arrived we emit the
   * whole character; only the continuation-only tail gets held). */
  char   utf8_pending[3];
  size_t utf8_pending_n;

  /* Most recently emitted byte - used to disambiguate `_` from
   * snake_case. Seeded '\n' so a leading `_` opens emphasis
   * (left boundary is bol / whitespace). */
  char last_char;
};

/**
 * @brief Initialize a renderer in-place. Zero-alloc.
 * @param md   Caller-owned storage.
 * @param sink Byte sink. Must not be NULL.
 * @param arg  Opaque user data forwarded to @p sink.
 */
XCAPI(void) xMdInit(xMd *md, xMdSinkFunc sink, void *arg);

/**
 * @brief Feed a chunk of markdown bytes. `len == 0` is a no-op.
 * @param md   The renderer.
 * @param data Chunk bytes (not required to be NUL-terminated).
 * @param len  Byte count.
 */
XCAPI(void) xMdFeed(xMd *md, const char *data, size_t len);

/**
 * @brief Drain pending state and close any open SGR spans.
 *        Idempotent.
 */
XCAPI(void) xMdFlush(xMd *md);

/**
 * @brief Discard pending bytes and emit an SGR reset without
 *        closing structural state. Used on mid-stream cancel so
 *        no style leaks into the next prompt.
 */
XCAPI(void) xMdReset(xMd *md);

#endif /* XTUI_MD_H */
