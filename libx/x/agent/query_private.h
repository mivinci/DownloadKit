/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_private.h - Internal layout and API of xAgentQuery_
 *
 * Exposed only to query.c, session.c and their tests. Other xai
 * translation units must go through query.h.
 *
 * A Query owns exactly the transient state of one in-flight run of
 * the provider/tool loop. Durable state (conversation history,
 * configuration, agent handle, caller callbacks) stays on xAgentSession_.
 *
 * Phase β: the Query is stateless w.r.t. the Session's history. On
 * xAgentQueryCreate the Session hands in a @c xAgentQueryConf; on
 * xAgentQueryRun the Session hands in the complete message array the
 * Query should run on (System prompt + rolling history + the new
 * user message). The Query deep-copies that array into @c inputs
 * and, as rounds progress, appends its own output (assistant turns,
 * tool_result entries) into @c produced. view_build walks
 * @c inputs + @c produced in order and hands the concatenation to
 * the provider. When the terminal on_done fires the Session pulls
 * @c produced out (into its rolling history) and destroys the
 * Query. The Query never reads or writes xAgentSession_::history.
 */

#ifndef XAGENT_QUERY_PRIVATE_H
#define XAGENT_QUERY_PRIVATE_H

#include <stddef.h>

#include <x/agent/message.h>  /* xAgentMessage                                */
#include <x/agent/provider.h> /* xAgentProviderStopReason, xAgentUsage           */
#include <x/agent/query.h>    /* xAgentQueryCallbacks                         */
#include <x/agent/session.h>  /* xAgentDoneReason                             */
#include <x/base/array.h>
#include <x/base/error.h>
#include <x/buf/buf.h> /* xBuffer                                    */

#include "turn_private.h" /* struct xAgentSessionMsg_                     */

/* Forward decl: Query holds a non-owning back-pointer to its owning
 * Session. The definition lives in session_private.h. */
struct xAgentSession_;

/**
 * @brief One pending tool call captured from the provider stream.
 *
 * query.c buffers the whole set while the assistant turn is still
 * streaming (tool_use arrives as part of the stream, but dispatch
 * must wait until on_done(ToolUse) so we know the assistant message
 * is complete and every call has its full arguments string).
 */
struct xAgentQueryPending_ {
  char *id;        /* tool_use_id supplied by the provider        */
  char *name;      /* tool name                                    */
  char *args_json; /* arguments JSON (owned copy)                  */
};

/**
 * @brief One asynchronously executing tool invocation.
 *
 * When a tool handler returns xErrno_Pending, query.c creates one
 * of these to track the in-flight operation. The on_done_fn callback
 * will resolve it by producing the final tool_result entry into
 * @c produced_arr.
 *
 * Lifetime: allocated in dispatch_pending_tools when xErrno_Pending
 * is returned; released when the on_done_fn callback fires (either
 * by appending to produced_arr or by error). The async_pending_arr
 * holds all live entries; once it is empty and no more synchronous
 * pending calls remain, the tool-loop continuation can proceed.
 */
struct xAgentQueryAsyncTool_ {
  xAgentTool          tool;      /* the tool handle (borrowed from tools[]) */
  xAgentToolDoneFunc  done_fn;   /* tool's on_done_fn (may be NULL)         */
  void            *done_ud;   /* tool's on_done_ud (may be NULL)        */
  xAgentToolCancelFunc cancel_fn; /* tool's on_cancel_fn (may be NULL)     */
  void            *cancel_ud; /* tool's on_cancel_ud (may be NULL)      */
  char            *id;        /* tool_use_id (owned copy)                */
  char            *name;      /* tool name (owned copy)                  */
  char            *args_json; /* arguments JSON (owned copy)              */

  /* Stage — distinguishes entries still waiting for a user
   * confirmation decision from ones whose handler is actually in
   * flight. See query.c:dispatch_pending_tools for the state
   * transitions. */
  int stage; /* 0 = AwaitingConfirm, 1 = Running                           */

  /* When stage == AwaitingConfirm the resolver handle that
   * xAgentToolConfirmResolve() closes over. Owned by the
   * async-pending array (freed on remove / release). NULL once the
   * entry transitions to Running.
   *
   * Back-pointer (struct xAgentToolConfirmResolver_ *) is declared as
   * void here so this header stays free of the resolver struct's
   * concrete layout, which is a query.c-private implementation
   * detail. */
  void *resolver;
};

/* Internal stage values for xAgentQueryAsyncTool_::stage. */
#define XAGENT_ASYNC_STAGE_AWAITING_CONFIRM 0
#define XAGENT_ASYNC_STAGE_RUNNING          1

/**
 * @brief One run of the agent tool loop.
 *
 * Lifetime: allocated by xAgentQueryCreate, started by xAgentQueryRun,
 * released by xAgentQueryDestroy. A Query is single-shot: at most one
 * Run per Query. The Session that hosts the Query is responsible
 * for issuing Destroy once the terminal on_done has fired (usually
 * from its forwarding on_done callback).
 *
 * Invariants:
 *   - @c running == 1 from xAgentQueryRun accepting the input until
 *     the terminal on_done has been delivered; 0 before and after.
 *   - @c cancelled == 1 once ai_query_cancel_mark runs; the loop
 *     unwinds to Aborted at the next natural boundary.
 *   - @c turn counts provider submits in this run (>=1 once the
 *     first round is in flight).
 *   - @c usage / @c saw_usage is the running total folded across
 *     every round of this run; surfaced to the caller via on_done.
 */
struct xAgentQuery_ {
  /* ── Self-contained runtime configuration ───────────────────────
   *
   * Stamped at xAgentQueryCreate from xAgentQueryConf and never mutated
   * afterwards. The Query no longer reaches back into a Session for
   * any of this — it is fully standalone. The only use for the
   * @c session pointer is the observational xAgentQuerySession() API;
   * it is never dereferenced for configuration. */
  xAgentProvider     provider;    /* borrowed, never NULL for a functional Query */
  const xAgentTool **tools;       /* borrowed array, may be NULL               */
  size_t          tools_count; /* number of entries in tools                */
  const char     *model;       /* borrowed, may be NULL                     */
  int             max_tokens;  /* per-round cap, 0 = provider default       */
  int             max_turns;   /* tool-loop cap, 0 = library default        */
  struct xAgentSession_ *session; /* optional, for xAgentQuerySession() only      */

  /* Streaming callbacks for this Query. Captured by value at
   * xAgentQueryCreate; callbacks fire with the Query handle (not the
   * Session) and the user_data stored in this struct. The Session
   * layer wires this up to its own forwarding shims. */
  xAgentQueryCallbacks cbs;

  /* ── Run input: the messages this Query is running on ─────────
   *
   * Handed to xAgentQueryRun by the Session layer and deep-copied into
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
  xBuffer assist; /* lazy-created on first append; NULL when unused */

  /* ── Assistant reasoning / thinking accumulator (current round) ── */
  xBuffer reasoning; /* lazy-created on first append; NULL when unused */

  /* ── Pending tool calls captured during the current round ─────── */
  xArray pending_arr;

  /* ── Asynchronously executing tool invocations ────────────────── */
  xArray async_pending_arr; /* holds struct xAgentQueryAsyncTool_ */

  /* ── Run-wide state ───────────────────────────────────────────── */
  int running;   /* 1 from Run until terminal on_done has fired    */
  int cancelled; /* ai_query_cancel_mark() sets this               */
  int turn;      /* number of provider submits this run (>=1)      */

  /* Cumulative token usage across every provider round of this run.
   * Each on_provider_done folds the round's per-request numbers in;
   * the terminal handler hands a pointer to this struct (or NULL if
   * no round ever reported usage) to the caller's on_done. We treat
   * -1 as "unknown" on input AND on output; the accumulator keeps
   * that sentinel intact until the first field we can actually fold.
   *
   * Note: prompt_tokens is accumulated via MAX, not addition — each
   * round reports the full input size the provider saw (not a delta),
   * so the maximum across rounds is the total input. completion_tokens
   * and total_tokens are additive (per-round deltas). */
  int      saw_usage; /* 1 once any round reported usage           */
  xAgentUsage usage;     /* running totals (prompt=max, others=add)   */

  /* The prompt_tokens reported by the provider on the FIRST round
   * of this run, or -1 if never reported. Used by the calibrator
   * to compare against the gate's pre-submit estimate — the gate
   * runs before any tool-loop rounds, so first-round is the only
   * value that maps cleanly to last_prompt_estimate. */
  int first_round_prompt_tokens;

  /* Free-list link: reused when the Query is returned to the
   * per-thread free list. Only valid when the Query is NOT in
   * active use (i.e. after xAgentQueryDestroy puts it back). */
  struct xAgentQuery_ *next;
};

/* ── Internal API (consumed by session.c) ────────────────────────── */

/**
 * @brief Submit one provider round over @c inputs + @c produced.
 *
 * Precondition: @c q->running == 1 and the input list has been
 * populated. Provider callbacks will fire later; the run terminates
 * when the Query's on_done has been delivered.
 *
 * Exposed as an entry point separate from xAgentQueryRun so that the
 * tool-loop continuation path (which has already populated
 * @c produced with the previous round's tool_results) can submit
 * without going through the Run front door again.
 *
 * @return xErrno_Ok if the submit was accepted; otherwise an error
 *         and no callbacks will fire.
 */
xErrno ai_query_submit(struct xAgentQuery_ *q);

/**
 * @brief Mark the Query as cancelled.
 *
 * The provider is told to stop; provider callbacks will eventually
 * reach the terminal handler with xAgentDoneReason_Aborted. Safe to
 * call when already cancelled or not running.
 */
void ai_query_cancel_mark(struct xAgentQuery_ *q);

/**
 * @brief Return the Query's @c produced list for merging.
 *
 * Returns a pointer into the xArray's internal storage and the
 * element count. The caller (session.c) must consume the data
 * BEFORE calling xAgentQueryDestroy, which will release every
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
void ai_query_take_produced(struct xAgentQuery_ *q, struct xAgentSessionMsg_ **out,
                            size_t *n_out);

/**
 * @brief Resolve one asynchronously executing tool invocation.
 *
 * Called from the tool's on_done_fn callback when an async tool
 * (one that returned xErrno_Pending from its handler) completes.
 * The function removes the matching entry from the Query's async
 * pending list, appends the tool_result to @c produced, and
 * checks whether all async tools for the current round are done
 * so the tool-loop can continue with the next provider round.
 *
 * @param q            Query handle.
 * @param tool_use_id  The tool_use_id that identifies the pending call.
 * @param result       The tool result content block. May be ToolResult
 *                     or Text; if NULL or unrecognized, an error result
 *                     is synthesized.
 */
void ai_query_async_tool_complete(struct xAgentQuery_ *q, const char *tool_use_id,
                                  const xAgentContent *result);

#endif /* XAGENT_QUERY_PRIVATE_H */
