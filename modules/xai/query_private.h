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
 * the provider/tool loop. Durable state (conversation history,
 * configuration, agent handle, caller callbacks) stays on xAiSession_.
 *
 * Phase β: the Query is stateless w.r.t. the Session's history. On
 * xAiQueryCreate the Session hands in a @c xAiQueryConf; on
 * xAiQueryRun the Session hands in the complete message array the
 * Query should run on (System prompt + rolling history + the new
 * user message). The Query deep-copies that array into @c inputs
 * and, as rounds progress, appends its own output (assistant turns,
 * tool_result entries) into @c produced. view_build walks
 * @c inputs + @c produced in order and hands the concatenation to
 * the provider. When the terminal on_done fires the Session pulls
 * @c produced out (into its rolling history) and destroys the
 * Query. The Query never reads or writes xAiSession_::history.
 */

#ifndef XAI_QUERY_PRIVATE_H
#define XAI_QUERY_PRIVATE_H

#include <stddef.h>

#include <xai/message.h>  /* xAiMessage                                */
#include <xai/provider.h> /* xAiProviderStopReason, xAiUsage           */
#include <xai/query.h>    /* xAiQueryCallbacks                         */
#include <xai/session.h>  /* xAiDoneReason                             */
#include <xbase/error.h>
#include <xbase/array.h>
#include <xbuf/buf.h>    /* xBuffer                                    */

#include "turn_private.h" /* struct xAiSessionMsg_                     */

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
   * provider callbacks to reach agent-level configuration (provider,
   * tools, model). History is NO LONGER read or written through this
   * pointer: the Query runs off its own @c inputs + @c produced
   * turn-entry arrays below and the Session merges @c produced back
   * into its history after the run terminates. */
  struct xAiSession_ *session;

  /* Streaming callbacks for this Query. Captured by value at
   * xAiQueryCreate; callbacks fire with the Query handle (not the
   * Session) and the user_data stored in this struct. The Session
   * layer wires this up to its own forwarding shims. */
  xAiQueryCallbacks cbs;

  /* ── Run input: the messages this Query is running on ─────────
   *
   * Handed to xAiQueryRun by the Session layer and deep-copied into
   * Query-owned storage. Every entry is one turn-entry record (same
   * shape as Session history); the Query never mutates this list
   * after Run accepts it. view_build walks this list first, then
   * @c produced below. */
  xArray inputs_arr;

  /* ── Run output: turns emitted during this run ────────────────
   *
   * Grown in round order: each committed assistant turn (thinking +
   * text + tool_use entries) and each tool_result produced by
   * dispatch_pending_tools lands here. view_build walks this list
   * after @c inputs so follow-up rounds see the full conversation
   * so far. On terminal on_done the Session merges this list into
   * its history (copy or move, implementation detail) before the
   * Query is destroyed. */
  xArray produced_arr;

  /* ── Assistant text accumulator for the current round ─────────── */
  xBuffer assist;   /* lazy-created on first append; NULL when unused */

  /* ── Assistant reasoning / thinking accumulator (current round) ── */
  xBuffer reasoning; /* lazy-created on first append; NULL when unused */

  /* ── Pending tool calls captured during the current round ─────── */
  xArray pending_arr;

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

  /* Free-list link: reused when the Query is returned to the
   * per-thread free list. Only valid when the Query is NOT in
   * active use (i.e. after xAiQueryDestroy puts it back). */
  struct xAiQuery_ *next;
};

/* ── Internal API (consumed by session.c) ────────────────────────── */

/**
 * @brief Submit one provider round over @c inputs + @c produced.
 *
 * Precondition: @c q->running == 1 and the input list has been
 * populated. Provider callbacks will fire later; the run terminates
 * when the Query's on_done has been delivered.
 *
 * Exposed as an entry point separate from xAiQueryRun so that the
 * tool-loop continuation path (which has already populated
 * @c produced with the previous round's tool_results) can submit
 * without going through the Run front door again.
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

/**
 * @brief Return the Query's @c produced list for merging.
 *
 * Returns a pointer into the xArray's internal storage and the
 * element count. The caller (session.c) must consume the data
 * BEFORE calling xAiQueryDestroy, which will release every
 * element via the array's release callback.
 *
 * Unlike the pre-xArray version this does NOT transfer ownership
 * of the outer buffer — the xArray still owns it.
 *
 * @param q      Query handle.
 * @param out    Receives the base pointer (may be NULL if the run
 *               produced nothing).
 * @param n_out  Receives the number of entries.
 */
void ai_query_take_produced(struct xAiQuery_ *q, struct xAiSessionMsg_ **out,
                            size_t *n_out);

#endif /* XAI_QUERY_PRIVATE_H */
