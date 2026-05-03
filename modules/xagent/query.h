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
 * Relationship to xAgentSession:
 *   - The Session owns the rolling conversation history and the
 *     long-lived configuration derived from the Agent.
 *   - A Query owns the transient state that lives exactly as long
 *     as "the model is running right now" (streaming accumulators,
 *     pending tool calls, per-run usage totals, cancellation flag).
 *   - A Session hosts Queries sequentially: every xAgentSessionInput()
 *     call allocates a fresh Query under the hood, runs it, and
 *     releases it when its terminal on_done has fired.
 *
 * Driving vs. observing:
 *   - The driving API (xAgentQueryCreate / xAgentQueryRun / xAgentQueryDestroy)
 *     is intended for the Session layer (and for future Agent-layer
 *     code that wants to inject SystemSynthesized queries alongside
 *     user-initiated ones). Regular application callers drive through
 *     xAgentSessionInput/Cancel.
 *   - The observing API (xAgentQueryIsRunning, xAgentQueryUsage,
 *     xAgentQueryTurn, xAgentQuerySession, xAgentSessionQuery) is free to use
 *     from any layer; it reads back state without side effects.
 *
 * Single-flight (today):
 *   - A Session accepts at most one live Query at a time. Calling
 *     xAgentSessionInput while the previous Query is still running
 *     returns xErrno_Busy. Creating a second Query on a Session that
 *     already has one also fails. The restriction is a Session-level
 *     invariant, not a Query-level one — the Query API itself does
 *     not assume it and a future Session variant may relax it
 *     (e.g. "real user input preempts an in-flight SystemSynthesized
 *     context-compression Query").
 *
 * Lifetime & ownership:
 *   - xAgentQueryCreate returns an owned handle; xAgentQueryDestroy must be
 *     called exactly once. Destroying a Query that still has a run
 *     in flight cancels it implicitly and drains pending callbacks
 *     before returning.
 *   - Query handles do NOT outlive their owning Session. If the
 *     Session is torn down while a Query is still live, the Session
 *     destroys the Query first.
 *
 * Threading:
 *   - Every xAgentQuery* call must happen on the owning agent's event
 *     loop, same as xAgentSession* calls. Callbacks fire on that loop.
 */

#ifndef XAGENT_QUERY_H
#define XAGENT_QUERY_H

#include <stddef.h>
#include <xagent/agent.h>    /* xAgent, xAgentTool, xAgentProvider, xAgentUsage */
#include <xagent/message.h>  /* xAgentMessage                               */
#include <xbase/base.h>
#include <xbase/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a session instance (forward declaration).
 *
 * Full definition lives in <xagent/session.h>. The Query only stores
 * this as an observational back-pointer; it never dereferences it.
 */
XDEF_HANDLE(xAgentSession);

/**
 * @brief User decision for a tool call awaiting confirmation.
 *
 * Delivered via xAgentToolConfirmResolve(). The tool confirm gate
 * is query-level and governed by xAgentTool::needs_confirm — only
 * tools flagged as needing confirmation trigger the gate; everything
 * else dispatches as before.
 */
XDEF_ENUM(xAgentToolDecision){
  /** Run the tool handler as usual. */
  xAgentToolDecision_Allow  = 0,
  /** Refuse the tool call. The agent fabricates an is_error=1
   *  tool_result carrying @p reason (or a default message when NULL)
   *  and feeds it back to the model, so the model knows the user
   *  declined and can change course. */
  xAgentToolDecision_Reject = 1,
};

/**
 * @brief Opaque handle for deferring a tool confirmation decision.
 *
 * Passed to xAgentQueryCallbacks::on_tool_confirm (and its session
 * forwarder xAgentSessionCallbacks::on_tool_confirm). The host keeps
 * the resolver across UI interaction and eventually calls
 * xAgentToolConfirmResolve() exactly once to deliver the decision.
 *
 * Threading: the handle is tied to the owning query's event loop.
 * Only call xAgentToolConfirmResolve() from that loop thread.
 *
 * Lifetime: the resolver is valid from the on_tool_confirm call
 * until it is resolved or the owning run is cancelled / destroyed,
 * whichever comes first. Resolving a stale resolver (e.g. after the
 * query was cancelled) is a silent no-op — the underlying tool
 * call is already gone.
 */
XDEF_HANDLE(xAgentToolConfirmResolver);

/**
 * @brief Submit a decision for a pending tool confirmation.
 *
 * Must be called exactly once per resolver. A second call on the
 * same handle is a silent no-op.
 *
 * @param r         The resolver handed to the host via
 *                  on_tool_confirm.
 * @param decision  Allow or Reject.
 * @param reason    Optional human-readable message. Only meaningful
 *                  for Reject — it becomes the text of the
 *                  synthetic tool_result delivered to the model.
 *                  May be NULL (default: "rejected by user").
 */
XCAPI(void) xAgentToolConfirmResolve(xAgentToolConfirmResolver r,
                                     xAgentToolDecision        decision,
                                     const char               *reason);

/**
 * @brief Why the Query's current run stopped.
 *
 * Delivered to the caller via xAgentQueryCallbacks::on_done (and
 * forwarded to xAgentSessionCallbacks::on_done when the Query is
 * Session-hosted). This is the coarse, caller-facing signal; the
 * Query layer internally maintains a finer-grained state machine
 * that is not exposed.
 */
XDEF_ENUM(xAgentDoneReason){
  xAgentDoneReason_Completed     = 0, /**< Run finished naturally        */
  xAgentDoneReason_MaxTurns      = 1, /**< Agent's max_turns reached     */
  xAgentDoneReason_PromptTooLong = 2, /**< Context budget exhausted      */
  xAgentDoneReason_Aborted       = 3, /**< xAgentQueryCancel() was called   */
  xAgentDoneReason_ModelError    = 4, /**< Provider/model returned error */
  xAgentDoneReason_ToolError     = 5, /**< Tool handler returned error   */
  xAgentDoneReason_Stopped       = 6, /**< Stopped by internal policy    */
};

/**
 * @brief Opaque handle to one run of the agent tool loop.
 *
 * A Query is created by xAgentQueryCreate, driven by xAgentQueryRun (which
 * may be called at most once today — the Query is single-shot; future
 * revisions may allow re-arming), and released by xAgentQueryDestroy.
 * Between Create and Run the Query is idle: xAgentQueryIsRunning() is 0.
 */
XDEF_HANDLE(xAgentQuery);

/**
 * @brief Streaming callbacks delivered during a Query run.
 *
 * Every callback is optional except @ref on_done. Any pointers passed
 * into callbacks are only valid for the duration of the call — copy
 * anything you need to retain. All callbacks run on the owning
 * agent's event loop thread. The shapes mirror xAgentSessionCallbacks so
 * the Session layer can forward one-to-one without adapting payloads.
 */
XDEF_STRUCT(xAgentQueryCallbacks) {
  /**
   * @brief Fired for each streamed assistant text chunk.
   *
   * @param q      The Query producing the token stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_text)(xAgentQuery q, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired for each streamed chain-of-thought ("thinking") chunk.
   *
   * Only reasoning-capable models emit these. See
   * xAgentSessionCallbacks::on_thinking for the full contract.
   *
   * @param q      The Query producing the thinking stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_thinking)(xAgentQuery q, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired exactly once when the Query terminates.
   *
   * Every successfully-started Query (i.e. xAgentQueryRun returned Ok)
   * produces exactly one on_done. Failure paths may additionally
   * fire on_error first as a diagnostic precursor; on_done is
   * always the authoritative terminator.
   *
   * @param q       The Query.
   * @param reason  Coarse completion reason (see xAgentDoneReason).
   * @param usage   Cumulative token usage across every provider
   *                round in this run, or NULL if the provider
   *                never reported any. Valid only for the duration
   *                of the callback.
   * @param ud      The user_data pointer from this struct.
   */
  void (*on_done)(xAgentQuery q, xAgentDoneReason reason, const xAgentUsage *usage,
                  void *ud);

  /**
   * @brief Fired as a diagnostic precursor before a failure on_done.
   *
   * See xAgentSessionCallbacks::on_error for the full contract. Not a
   * terminator — always followed by on_done.
   *
   * @param q    The Query.
   * @param err  xErrno describing the failure class.
   * @param msg  Human-readable diagnostic (may be NULL).
   * @param ud   The user_data pointer from this struct.
   */
  void (*on_error)(xAgentQuery q, xErrno err, const char *msg, void *ud);

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
  void (*on_tool)(xAgentQuery q, const char *tool_name, int started, void *ud);

  /**
   * @brief Optional: streaming output from an in-flight tool.
   *
   * Fired by async tools (e.g. shell) to deliver incremental output
   * before the final tool_result. May be NULL = caller does not want
   * streaming output.
   *
   * @param q           The Query.
   * @param tool_use_id The tool_use_id of the in-flight tool call.
   * @param tool_name   The registered tool name.
   * @param data        Output chunk (NOT NUL-terminated).
   * @param len         Length of @p data in bytes.
   * @param ud          The user_data pointer from this struct.
   */
  void (*on_tool_output)(xAgentQuery q, const char *tool_use_id,
                         const char *tool_name, const char *data, size_t len,
                         void *ud);

  /**
   * @brief Optional: user-confirmation gate for tools flagged as
   *        @ref xAgentTool::needs_confirm.
   *
   * Fires once per tool invocation whose tool handle was created
   * with @c needs_confirm != 0, immediately before the handler would
   * be dispatched. Tools that do NOT opt in never trigger this
   * callback — they run as usual.
   *
   * The host is expected to display a UI, ask the user, and then
   * call xAgentToolConfirmResolve() with the decision. While the
   * resolver is outstanding the owning query is paused: the tool
   * handler is not dispatched, on_tool(started=1) has not yet fired,
   * and no follow-up provider round is issued. Other tools in the
   * same batch that do NOT need confirmation are dispatched normally
   * in parallel.
   *
   * Multiple needs_confirm tool calls arriving in the same assistant
   * turn trigger on_tool_confirm once per call, each with its own
   * independent resolver.
   *
   * If this callback is NULL, all @c needs_confirm tools run without
   * asking — the gate is disabled by default.
   *
   * Cancellation: if the owning run is cancelled while a resolver
   * is outstanding, the pending call is synthesised as a cancelled
   * tool_result and the resolver becomes a silent no-op.
   *
   * @param q            The Query.
   * @param tool_name    The registered tool name.
   * @param tool_use_id  The provider-supplied tool_use_id for this call.
   * @param args_json    The JSON argument string the model supplied
   *                     (valid only for the duration of the call —
   *                     deep-copy if needed beyond the UI turn).
   * @param resolver     Opaque handle; pass to xAgentToolConfirmResolve.
   * @param ud           The user_data pointer from this struct.
   */
  void (*on_tool_confirm)(xAgentQuery q, const char *tool_name,
                          const char *tool_use_id, const char *args_json,
                          xAgentToolConfirmResolver resolver, void *ud);

  /** Forwarded to every callback in this struct. */
  void *user_data;
};

/**
 * @brief Configuration for creating a Query.
 *
 * Zero-initialise for defaults. The Query is self-contained: it
 * carries its own provider, tools, model, and limits — it does NOT
 * reach back into any Session for runtime configuration. The Session
 * layer fills these in from its resolved state before calling
 * xAgentQueryCreate; standalone callers fill them directly.
 *
 * The @ref session field is optional and purely observational: it
 * lets xAgentQuerySession() return the owning Session for callers that
 * need it, but the Query never dereferences it for configuration.
 */
XDEF_STRUCT(xAgentQueryConf) {
  xAgentQueryCallbacks cbs; /**< Streaming callbacks (in-place).          */

  /* ── Runtime configuration (self-contained, no Session back-hack) ── */

  /**
   * @brief LLM provider to submit rounds to.
   *
   * Borrowed for the Query's lifetime; must outlive the Query.
   * Must not be NULL for a functional Query.
   */
  xAgentProvider provider;

  /**
   * @brief Tool definitions advertised to the model.
   *
   * Borrowed array of tool handles; individual entries may be NULL.
   * May be NULL / tools_count == 0 when no tools are configured.
   */
  const xAgentTool **tools;
  size_t          tools_count;

  /**
   * @brief Model identifier sent to the provider.
   *
   * Borrowed; may be NULL (the provider's own default is used).
   */
  const char *model;

  /**
   * @brief Per-round token cap forwarded to the provider.
   *
   * Zero means "use the provider's default".
   */
  int max_tokens;

  /**
   * @brief Maximum number of provider rounds (tool-loop iterations).
   *
   * Zero means "use the library default" (currently 16, see
   * XAGENT_SESSION_DEFAULT_MAX_TURNS in session_private.h).
   */
  int max_turns;

  /* ── Optional observational back-pointer ────────────────────────── */

  /**
   * @brief Owning Session, if any.
   *
   * When the Query is created by a Session (via xAgentSessionInput),
   * this field is set so that xAgentQuerySession() can return it.
   * Standalone Queries leave this NULL. The Query never dereferences
   * this pointer for configuration — all runtime state is carried
   * in the fields above.
   */
  xAgentSession session;
};

/* ── Driving API ──────────────────────────────────────────────── */

/**
 * @brief Create an idle Query with self-contained configuration.
 *
 * The Query borrows the provider, tools, model, and session handle
 * from @p conf for its lifetime; they must outlive the Query.
 * @p conf is captured by value (including the callbacks struct), so
 * it may be stack-allocated.
 *
 * The Query starts idle — no provider round is submitted until
 * xAgentQueryRun is called.
 *
 * Single-flight constraint: if @p conf->session is non-NULL and
 * that Session already has a live Query (one that was created but
 * not yet Destroyed), this returns NULL. When @p conf->session is
 * NULL (standalone Query), no single-flight check is performed —
 * the caller is responsible for their own concurrency discipline.
 *
 * @param conf  Query configuration (must not be NULL).
 * @return      A new Query handle, or NULL on failure.
 */
XCAPI(xAgentQuery) xAgentQueryCreate(const xAgentQueryConf *conf);

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
XCAPI(xErrno) xAgentQueryRun(xAgentQuery q, const xAgentMessage *msgs, size_t n);

/**
 * @brief Release the Query.
 *
 * If a run is still in flight, cancellation is issued first and
 * the terminal on_done fires before the storage is freed. After
 * this call the handle is invalid.
 *
 * @param q  Query handle (NULL is a no-op).
 */
XCAPI(void) xAgentQueryDestroy(xAgentQuery q);

/* ── Observing API ────────────────────────────────────────────── */

/* xAgentSessionQuery is declared in <xagent/session.h>. It returns the
 * live Query (if any) on a Session handle, which is a Session-layer
 * concept. */

/**
 * @brief Cancel this Query's in-flight run, if any.
 *
 * Requests the provider to stop streaming and any in-flight tool
 * handlers to bail out. The on_done callback is still delivered
 * (with reason == xAgentDoneReason_Aborted) once unwinding completes.
 * A silent no-op on a NULL handle or a Query that is not running.
 *
 * @param q  Query handle (NULL is a no-op).
 */
XCAPI(void) xAgentQueryCancel(xAgentQuery q);

/**
 * @brief Whether this Query currently has a run in flight.
 *
 * Returns 1 between xAgentQueryRun accepting an input and the matching
 * on_done being delivered; 0 otherwise (idle, finished, NULL).
 *
 * @param q  Query handle.
 * @return   1 if a run is in flight, 0 otherwise.
 */
XCAPI(int) xAgentQueryIsRunning(xAgentQuery q);

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
XCAPI(void) xAgentQueryUsage(xAgentQuery q, xAgentUsage *out);

/**
 * @brief Read back the owning Session of this Query, if any.
 *
 * Returns the Session that was set in xAgentQueryConf::session at
 * creation time. Standalone Queries (where conf->session was NULL)
 * return NULL here.
 *
 * @param q  Query handle.
 * @return   The owning Session, or NULL.
 */
XCAPI(xAgentSession) xAgentQuerySession(xAgentQuery q);

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
XCAPI(int) xAgentQueryTurn(xAgentQuery q);

#ifdef __cplusplus
}
#endif

#endif /* XAGENT_QUERY_H */
