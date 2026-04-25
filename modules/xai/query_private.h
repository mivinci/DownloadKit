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

#include <xai/message.h>   /* xAiMessage (unused directly here but kept
                              for downstream includers that expect it) */
#include <xai/provider.h>  /* xAiProviderStopReason, xAiUsage           */
#include <xai/session.h>   /* xAiDoneReason                             */
#include <xbase/error.h>

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
 */
struct xAiQueryPending_ {
  char *id;        /* tool_use_id supplied by the provider        */
  char *name;      /* tool name                                    */
  char *args_json; /* arguments JSON (owned copy)                  */
};

/**
 * @brief One in-flight run of the agent tool loop.
 *
 * Lifetime: armed by the Session when xAiSessionInput is accepted,
 * reset when that run's terminal on_done has fired. The Session
 * owns the storage (embedded member, not a pointer); promotion to a
 * heap handle is a follow-up for when SystemSynthesized queries
 * start coexisting with user-initiated ones.
 *
 * Invariants:
 *   - @c running == 1 from ai_query_arm until ai_query_finalize has
 *     delivered the terminal on_done.
 *   - @c cancelled == 1 once ai_query_cancel_mark runs; the loop
 *     unwinds to Aborted at the next natural boundary.
 *   - @c turn counts provider submits in this run (>=1 once the
 *     first round is in flight).
 *   - @c usage / @c saw_usage is the running total folded across
 *     every round of this run; ai_query_finalize surfaces it.
 */
struct xAiQuery_ {
  /* Non-owning back-pointer to the Session that hosts this Query.
   * Set by ai_query_arm, cleared lazily (every subsequent run
   * re-arms and resets it). Used by provider callbacks to reach
   * history and the caller's xAiSessionCallbacks. */
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
  int running;   /* 1 from arm until finalize has fired on_done    */
  int cancelled; /* ai_query_cancel_mark() sets this               */
  int turn;      /* number of provider submits this run (>=1)      */

  /* Cumulative token usage across every provider round of this run.
   * Each on_provider_done folds the round's per-request numbers in;
   * ai_query_finalize hands a pointer to this struct (or NULL if no
   * round ever reported usage) to the caller's on_done. We treat -1
   * as "unknown" on input AND on output; the accumulator keeps that
   * sentinel intact until the first field we can actually add.
   * Reset by ai_query_arm. */
  int      saw_usage; /* 1 once any round reported usage           */
  xAiUsage usage;     /* running totals                            */
};

/* ── Internal API ────────────────────────────────────────────────── */

/**
 * @brief Arm a Query for a fresh run.
 *
 * Sets the back-pointer, clears running/cancelled/turn, and resets
 * the usage accumulator. Does NOT touch the assist / reasoning /
 * pending buffers — those are round-scoped and reset by submit.
 *
 * @param q        Query instance (must not be NULL).
 * @param session  Owning Session (must not be NULL).
 */
void ai_query_arm(struct xAiQuery_ *q, struct xAiSession_ *session);

/**
 * @brief Submit one provider round over the Session's current history.
 *
 * Precondition: @c q->running == 1. Caller is responsible for
 * appending the triggering user message / tool_results into history
 * before calling. On success, provider callbacks will fire later;
 * the run terminates when ai_query_finalize runs.
 *
 * @return xErrno_Ok if the submit was accepted; otherwise an error
 *         and no callbacks will fire.
 */
xErrno ai_query_submit(struct xAiQuery_ *q);

/**
 * @brief Mark the Query as cancelled.
 *
 * The provider is told to stop; provider callbacks will eventually
 * reach ai_query_finalize with xAiDoneReason_Aborted. Safe to call
 * when already cancelled or not running.
 */
void ai_query_cancel_mark(struct xAiQuery_ *q);

/**
 * @brief Release heap buffers owned by the Query.
 *
 * Called from xAiSessionDestroy after the run is known to be idle.
 * Does not free @p q itself (the Query is embedded in the Session).
 */
void ai_query_dispose(struct xAiQuery_ *q);

/**
 * @brief Reset the Query to its idle shape after a run terminates.
 *
 * Clears the round-scoped streaming buffers, the pending-call slab,
 * the usage accumulator, and the @c running / @c cancelled / @c turn
 * flags. Does NOT free the heap backing of @c assist_buf /
 * @c reasoning_buf / @c pending — those live across runs (next run
 * reuses the capacity). Called from ai_session_finish_run.
 */
void ai_query_reset_round(struct xAiQuery_ *q);

#endif /* XAI_QUERY_PRIVATE_H */
