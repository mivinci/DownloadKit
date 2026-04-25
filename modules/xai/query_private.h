/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_private.h - Internal layout and API of xAiQuery_
 *
 * Exposed only to query.c, session.c and their tests. Other xai
 * translation units must go through query.h.
 *
 * A Query owns exactly the transient state of one in-flight run of
 * the provider/tool loop. Durable state (history, configuration,
 * agent handle, caller callbacks) stays on xAiSession_. Each
 * accepted xAiSessionInput() spawns one heap-allocated Query via
 * xAiQueryCreate, runs it with xAiQueryRun, and destroys it with
 * xAiQueryDestroy once the terminal on_done has fired. The Query
 * keeps a back-pointer to its owning Session so provider callbacks
 * can reach history and agent-level configuration.
 */

#ifndef XAI_QUERY_PRIVATE_H
#define XAI_QUERY_PRIVATE_H

#include <stddef.h>

#include <xai/message.h>  /* xAiMessage                                */
#include <xai/provider.h> /* xAiProviderStopReason, xAiUsage           */
#include <xai/query.h>    /* xAiQueryCallbacks                         */
#include <xai/session.h>  /* xAiDoneReason                             */
#include <xbase/error.h>

/* Forward decl: Query holds a non-owning back-pointer to its owning
 * Session. The definition lives in session_private.h. */
struct xAiSession_;

/**
 * @brief One pending tool call captured from the provider stream.
 *
 * query.c buffers the whole set while the assistant turn is still
 * streaming (tool_use arrives as part of the stream, but dispatch
 * must wait until on_done(ToolUse) so we know the assistant message
 * is complete and every call has its full arguments string).
 */
struct xAiQueryPending_ {
  char *id;        /* tool_use_id supplied by the provider        */
  char *name;      /* tool name                                    */
  char *args_json; /* arguments JSON (owned copy)                  */
};

/**
 * @brief One run of the agent tool loop.
 *
 * Lifetime: allocated by xAiQueryCreate, started by xAiQueryRun,
 * released by xAiQueryDestroy. A Query is single-shot: at most one
 * Run per Query. The Session that hosts the Query is responsible
 * for issuing Destroy once the terminal on_done has fired (usually
 * from its forwarding on_done callback).
 *
 * Invariants:
 *   - @c running == 1 from xAiQueryRun accepting the input until
 *     the terminal on_done has been delivered; 0 before and after.
 *   - @c cancelled == 1 once ai_query_cancel_mark runs; the loop
 *     unwinds to Aborted at the next natural boundary.
 *   - @c turn counts provider submits in this run (>=1 once the
 *     first round is in flight).
 *   - @c usage / @c saw_usage is the running total folded across
 *     every round of this run; surfaced to the caller via on_done.
 */
struct xAiQuery_ {
  /* Non-owning back-pointer to the Session that hosts this Query.
   * Stamped at xAiQueryCreate and never mutated afterwards. Used by
   * provider callbacks to reach history and agent-level config. */
  struct xAiSession_ *session;

  /* Streaming callbacks for this Query. Captured by value at
   * xAiQueryCreate; callbacks fire with the Query handle (not the
   * Session) and the user_data stored in this struct. The Session
   * layer wires this up to its own forwarding shims. */
  xAiQueryCallbacks cbs;

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
  int running;   /* 1 from Run until terminal on_done has fired    */
  int cancelled; /* ai_query_cancel_mark() sets this               */
  int turn;      /* number of provider submits this run (>=1)      */

  /* Cumulative token usage across every provider round of this run.
   * Each on_provider_done folds the round's per-request numbers in;
   * the terminal handler hands a pointer to this struct (or NULL if
   * no round ever reported usage) to the caller's on_done. We treat
   * -1 as "unknown" on input AND on output; the accumulator keeps
   * that sentinel intact until the first field we can actually add. */
  int      saw_usage; /* 1 once any round reported usage           */
  xAiUsage usage;     /* running totals                            */
};

/* ── Internal API (consumed by session.c) ────────────────────────── */

/**
 * @brief Submit one provider round over the Session's current history.
 *
 * Precondition: @c q->running == 1. Caller is responsible for
 * appending the triggering user message / tool_results into history
 * before calling. On success, provider callbacks will fire later;
 * the run terminates when the Query's on_done has been delivered.
 *
 * Exposed here (rather than only via xAiQueryRun) because session.c
 * drives the append-history-then-submit ordering during initial
 * xAiSessionInput, where the Query has already been created but
 * history must be written before the submit happens. In Phase β,
 * once history is fully owned by the Session and Queries run off
 * explicit message arrays, this entry point goes away and callers
 * use xAiQueryRun exclusively.
 *
 * @return xErrno_Ok if the submit was accepted; otherwise an error
 *         and no callbacks will fire.
 */
xErrno ai_query_submit(struct xAiQuery_ *q);

/**
 * @brief Mark the Query as cancelled.
 *
 * The provider is told to stop; provider callbacks will eventually
 * reach the terminal handler with xAiDoneReason_Aborted. Safe to
 * call when already cancelled or not running.
 */
void ai_query_cancel_mark(struct xAiQuery_ *q);

#endif /* XAI_QUERY_PRIVATE_H */
