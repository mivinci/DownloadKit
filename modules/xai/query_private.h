/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_private.h - Internal layout of xAiQuery_
 *
 * Exposed only to query.c, session.c and their tests. Other xai
 * translation units must go through query.h.
 *
 * A Query owns exactly the transient state of one in-flight run of
 * the provider/tool loop. Durable state (history, configuration,
 * agent handle, caller callbacks) stays on xAiSession_. The two
 * structs reference each other: the Session embeds one Query
 * directly (we only ever run one at a time today), and the Query
 * keeps a back-pointer so its provider callbacks can reach the
 * owning history + caller cbs without threading yet another void*.
 */

#ifndef XAI_QUERY_PRIVATE_H
#define XAI_QUERY_PRIVATE_H

#include "query.h"

#include <stddef.h>

#include <xai/provider.h> /* xAiUsage */

/* Forward decl: Query holds a non-owning back-pointer to its owning
 * Session. The definition lives in session_private.h (which includes
 * this header to embed the Query). */
struct xAiSession_;

/**
 * @brief One pending tool call captured from the provider stream.
 *
 * query.c buffers the whole set while the assistant turn is still
 * streaming (tool_use arrives as part of the stream, but dispatch
 * must wait until on_done(ToolUse) so we know the assistant message
 * is complete and every call has its full arguments string).
 *
 * Moved here from session_private.h in Step 2 of the Session/Query
 * split: it belongs to the in-flight round, not to the Session.
 */
struct xAiQueryPending_ {
  char *id;        /* tool_use_id supplied by the provider        */
  char *name;      /* tool name                                    */
  char *args_json; /* arguments JSON (owned copy)                  */
};

/**
 * @brief One in-flight run of the agent tool loop.
 *
 * Lifetime: created lazily by the Session when xAiSessionInput is
 * accepted, torn down when that run's terminal on_done has fired.
 * The Session owns the storage (embedded member, not a pointer); a
 * future refactor may promote it to a heap handle once we actually
 * need more than one per Session.
 *
 * Invariants:
 *   - @c running == 1 from accept until finish_run has fired on_done.
 *   - @c cancelled == 1 means xAiSessionCancel was observed; the
 *     loop will unwind to Aborted at the next natural boundary.
 *   - @c turn is the number of provider submits already issued in
 *     this run (>=1 once the first round is in flight).
 *   - @c usage / @c saw_usage is the running total folded across
 *     every round of this run; finish_run surfaces it to on_done.
 */
struct xAiQuery_ {
  /* Non-owning back-pointer to the Session that hosts this Query.
   * Set when the Query is armed (by the Session) and cleared when
   * the run finishes. Used by provider callbacks to reach history
   * and the caller's xAiSessionCallbacks. */
  struct xAiSession_ *session;

  /* ── Assistant text accumulator for the current round ─────────── */
  char  *assist_buf;
  size_t assist_len;
  size_t assist_cap;

  /* ── Assistant reasoning / thinking accumulator (current round) ── */
  char  *reasoning_buf;
  size_t reasoning_len;
  size_t reasoning_cap;

  /* ── Pending tool calls captured during the current round ─────── */
  struct xAiQueryPending_ *pending;
  size_t                   n_pending;
  size_t                   cap_pending;

  /* ── Run-wide state ───────────────────────────────────────────── */
  int running;   /* 1 from Input accept to final on_done           */
  int cancelled; /* xAiSessionCancel() sets this                   */
  int turn;      /* number of provider submits this run (>=1)      */

  /* Cumulative token usage across every provider round of this run.
   * Each on_provider_done folds the round's per-request numbers in;
   * finish_run hands a pointer to this struct (or NULL if no round
   * ever reported usage) to the caller's on_done. We treat -1 as
   * "unknown" on input AND on output; the accumulator keeps that
   * sentinel intact until the first field we can actually add.
   * Reset to all-(-1) at the start of every run. */
  int      saw_usage; /* 1 once any round reported usage           */
  xAiUsage usage;     /* running totals                            */
};

#endif /* XAI_QUERY_PRIVATE_H */
