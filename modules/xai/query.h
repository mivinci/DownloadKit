/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.h - One run of the xai tool loop
 *
 * A Query represents a single end-to-end execution of the provider
 * tool loop:
 *
 *     input ──▶ provider round 1 ──▶ (tools?) ──▶ round 2 ... ──▶ on_done
 *
 * Relationship to xAiSession:
 *   - The Session owns the rolling conversation history and the
 *     long-lived configuration derived from the Agent.
 *   - A Query owns the transient state that lives exactly as long
 *     as "the model is running right now" (streaming accumulators,
 *     pending tool calls, per-run usage totals, cancellation flag).
 *   - A Session hosts Queries sequentially: every xAiSessionInput()
 *     call allocates a fresh Query under the hood, runs it, and
 *     releases it when its terminal on_done has fired.
 *
 * Driving vs. observing:
 *   - The driving API (xAiQueryCreate / xAiQueryRun / xAiQueryDestroy)
 *     is intended for the Session layer (and for future Agent-layer
 *     code that wants to inject SystemSynthesized queries alongside
 *     user-initiated ones). Regular application callers drive through
 *     xAiSessionInput/Cancel.
 *   - The observing API (xAiQueryIsRunning, xAiQueryUsage,
 *     xAiQueryTurn, xAiQuerySession, xAiSessionQuery) is free to use
 *     from any layer; it reads back state without side effects.
 *
 * Single-flight (today):
 *   - A Session accepts at most one live Query at a time. Calling
 *     xAiSessionInput while the previous Query is still running
 *     returns xErrno_Busy. Creating a second Query on a Session that
 *     already has one also fails. The restriction is a Session-level
 *     invariant, not a Query-level one — the Query API itself does
 *     not assume it and a future Session variant may relax it
 *     (e.g. "real user input preempts an in-flight SystemSynthesized
 *     context-compression Query").
 *
 * Lifetime & ownership:
 *   - xAiQueryCreate returns an owned handle; xAiQueryDestroy must be
 *     called exactly once. Destroying a Query that still has a run
 *     in flight cancels it implicitly and drains pending callbacks
 *     before returning.
 *   - Query handles do NOT outlive their owning Session. If the
 *     Session is torn down while a Query is still live, the Session
 *     destroys the Query first.
 *
 * Threading:
 *   - Every xAiQuery* call must happen on the owning agent's event
 *     loop, same as xAiSession* calls. Callbacks fire on that loop.
 */

#ifndef XAI_QUERY_H
#define XAI_QUERY_H

#include <stddef.h>
#include <xai/message.h>  /* xAiMessage                                */
#include <xai/provider.h> /* xAiUsage                                  */
#include <xai/session.h>  /* xAiSession, xAiInputOrigin, xAiDoneReason */
#include <xbase/base.h>
#include <xbase/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to one run of the agent tool loop.
 *
 * A Query is created by xAiQueryCreate, driven by xAiQueryRun (which
 * may be called at most once today — the Query is single-shot; future
 * revisions may allow re-arming), and released by xAiQueryDestroy.
 * Between Create and Run the Query is idle: xAiQueryIsRunning() is 0.
 */
XDEF_HANDLE(xAiQuery);

/**
 * @brief Streaming callbacks delivered during a Query run.
 *
 * Every callback is optional except @ref on_done. Any pointers passed
 * into callbacks are only valid for the duration of the call — copy
 * anything you need to retain. All callbacks run on the owning
 * agent's event loop thread. The shapes mirror xAiSessionCallbacks so
 * the Session layer can forward one-to-one without adapting payloads.
 */
XDEF_STRUCT(xAiQueryCallbacks) {
  /**
   * @brief Fired for each streamed assistant text chunk.
   *
   * @param q      The Query producing the token stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_text)(xAiQuery q, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired for each streamed chain-of-thought ("thinking") chunk.
   *
   * Only reasoning-capable models emit these. See
   * xAiSessionCallbacks::on_thinking for the full contract.
   *
   * @param q      The Query producing the thinking stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_thinking)(xAiQuery q, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired exactly once when the Query terminates.
   *
   * Every successfully-started Query (i.e. xAiQueryRun returned Ok)
   * produces exactly one on_done. Failure paths may additionally
   * fire on_error first as a diagnostic precursor; on_done is
   * always the authoritative terminator.
   *
   * @param q       The Query.
   * @param reason  Coarse completion reason (see xAiDoneReason).
   * @param usage   Cumulative token usage across every provider
   *                round in this run, or NULL if the provider
   *                never reported any. Valid only for the duration
   *                of the callback.
   * @param ud      The user_data pointer from this struct.
   */
  void (*on_done)(xAiQuery q, xAiDoneReason reason, const xAiUsage *usage,
                  void *ud);

  /**
   * @brief Fired as a diagnostic precursor before a failure on_done.
   *
   * See xAiSessionCallbacks::on_error for the full contract. Not a
   * terminator — always followed by on_done.
   *
   * @param q    The Query.
   * @param err  xErrno describing the failure class.
   * @param msg  Human-readable diagnostic (may be NULL).
   * @param ud   The user_data pointer from this struct.
   */
  void (*on_error)(xAiQuery q, xErrno err, const char *msg, void *ud);

  /**
   * @brief Optional: coarse tool-execution progress signal.
   *
   * Delivered twice per tool invocation: @p started == 1 before
   * the handler runs, @p started == 0 after.
   *
   * @param q          The Query.
   * @param tool_name  The registered tool name.
   * @param started    Non-zero = started; zero = finished.
   * @param ud         The user_data pointer from this struct.
   */
  void (*on_tool)(xAiQuery q, const char *tool_name, int started, void *ud);

  /** Forwarded to every callback in this struct. */
  void *user_data;
};

/**
 * @brief Configuration for creating a Query.
 *
 * Zero-initialise to inherit everything the owning Session already
 * knows about. Today the only field is @ref cbs; future revisions
 * will add overrides specific to the Query layer (tool-allow list,
 * per-Query max_turns, reasoning effort, ...).
 */
XDEF_STRUCT(xAiQueryConf) {
  xAiQueryCallbacks cbs; /**< Streaming callbacks (in-place).          */
};

/* ── Driving API ──────────────────────────────────────────────── */

/**
 * @brief Create an idle Query bound to the given Session.
 *
 * The Query borrows @p sess for its lifetime; @p sess must outlive
 * the Query. @p conf is captured by value (including the callbacks
 * struct), so it may be stack-allocated.
 *
 * The Query starts idle — no provider round is submitted until
 * xAiQueryRun is called.
 *
 * Single-flight constraint: if @p sess already has a live Query (one
 * that was created but not yet Destroyed), this returns NULL. See
 * the header-level comment on single-flight for the rationale.
 *
 * @param sess  Session handle (must not be NULL).
 * @param conf  Query configuration (must not be NULL).
 * @return      A new Query handle, or NULL on failure.
 */
XCAPI(xAiQuery) xAiQueryCreate(xAiSession sess, const xAiQueryConf *conf);

/**
 * @brief Start the Query by running it against an explicit message list.
 *
 * Deep-copies @p msgs into Query-owned storage and kicks off the
 * first provider round. Streaming callbacks begin firing
 * asynchronously; the run terminates with exactly one on_done.
 *
 * The message array is fully consumed at call time: the Query copies
 * every role/content block into its own storage, so the caller may
 * free @p msgs (and every string it points into) as soon as this
 * function returns.
 *
 * The Query does NOT read back from or write into its owning
 * Session's rolling history — that is the Session layer's
 * responsibility. What the Query produces during the run (assistant
 * turns, tool_result entries) can be pulled out with
 * ai_query_take_produced() after on_done has fired.
 *
 * Must be called at most once per Query. Calling it twice returns
 * xErrno_Busy.
 *
 * @param q     Query handle (must not be NULL).
 * @param msgs  Message array the Query should run on. Must include
 *              any system prompt and prior conversation history the
 *              caller wants the provider to see. May be NULL iff
 *              @p n is 0, in which case xErrno_InvalidArg is returned
 *              (no content to send).
 * @param n     Number of messages in @p msgs.
 * @return      xErrno_Ok if the submit was accepted; xErrno_Busy
 *              if the Query is already running or finished;
 *              xErrno_InvalidArg on empty input; other xErrno on
 *              allocation failure. On non-Ok returns no callbacks
 *              will fire.
 */
XCAPI(xErrno) xAiQueryRun(xAiQuery q, const xAiMessage *msgs, size_t n);

/**
 * @brief Release the Query.
 *
 * If a run is still in flight, cancellation is issued first and
 * the terminal on_done fires before the storage is freed. After
 * this call the handle is invalid.
 *
 * @param q  Query handle (NULL is a no-op).
 */
XCAPI(void) xAiQueryDestroy(xAiQuery q);

/* ── Observing API ────────────────────────────────────────────── */

/**
 * @brief Get the Query currently running on a Session, if any.
 *
 * Returns the live Query handle the Session has allocated for its
 * in-flight run, or NULL if the Session is idle. The handle stays
 * valid until either on_done has been delivered (at which point
 * the Session will destroy it) or xAiSessionDestroy() is called.
 *
 * @param sess  Session handle.
 * @return      The Session's current Query, or NULL if none.
 */
XCAPI(xAiQuery) xAiSessionQuery(xAiSession sess);

/**
 * @brief Cancel this Query's in-flight run, if any.
 *
 * Requests the provider to stop streaming and any in-flight tool
 * handlers to bail out. The on_done callback is still delivered
 * (with reason == xAiDoneReason_Aborted) once unwinding completes.
 * A silent no-op on a NULL handle or a Query that is not running.
 *
 * @param q  Query handle (NULL is a no-op).
 */
XCAPI(void) xAiQueryCancel(xAiQuery q);

/**
 * @brief Whether this Query currently has a run in flight.
 *
 * Returns 1 between xAiQueryRun accepting an input and the matching
 * on_done being delivered; 0 otherwise (idle, finished, NULL).
 *
 * @param q  Query handle.
 * @return   1 if a run is in flight, 0 otherwise.
 */
XCAPI(int) xAiQueryIsRunning(xAiQuery q);

/**
 * @brief Cumulative token usage across every provider round so far.
 *
 * Copies the running totals into @p out. Each field uses -1 as the
 * "unknown" sentinel. Before any round reports usage every field
 * is -1; between rounds the totals accumulate.
 *
 * @param q    Query handle (NULL populates @p out with all -1).
 * @param out  Destination struct (must not be NULL).
 */
XCAPI(void) xAiQueryUsage(xAiQuery q, xAiUsage *out);

/**
 * @brief Read back the owning Session of this Query.
 *
 * @param q  Query handle.
 * @return   The owning Session, or NULL for a NULL input.
 */
XCAPI(xAiSession) xAiQuerySession(xAiQuery q);

/**
 * @brief Provider round counter for this run.
 *
 * Incremented just before each submit: reads 1 during the first
 * round's callbacks, 2 during the second round, and so on. Reads 0
 * on a NULL or idle (not-yet-run) handle.
 *
 * @param q  Query handle.
 * @return   Number of provider submits issued so far in this run.
 */
XCAPI(int) xAiQueryTurn(xAiQuery q);

#ifdef __cplusplus
}
#endif

#endif /* XAI_QUERY_H */
