/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session.h - One conversation instance for the xai agent core
 *
 * An xAgentSession is a single live conversation derived from an agent.
 * It owns the rolling message history, drives the agent loop
 * (prompt → stream → maybe tool calls → next round), and surfaces
 * streaming events to the caller through a small set of callbacks.
 *
 * Relationship to xAgent:
 * - The agent is a long-lived capability template (provider, tools,
 *   model defaults, limits).
 * - A session is short-lived and specific to one chat. One agent
 *   may spawn many concurrent sessions on the same event loop.
 * - Fields of xAgentSessionConf override the agent's defaults for this
 *   session only; leaving them zero means "inherit from the agent".
 *
 * Visibility philosophy:
 * - The caller sees tokens (on_text), completion (on_done), errors
 *   (on_error), and coarse tool progress (on_tool, optional). All
 *   internal machinery – Terminal / Continue state transitions,
 *   tool_use / tool_result content block routing, prompt
 *   compression – is deliberately hidden and may evolve without
 *   breaking this API.
 *
 * Threading:
 * - Every xAgentSession* call must happen on the session's agent event
 *   loop. Callbacks are invoked on that same loop. Pointers passed
 *   into callbacks are valid only for the duration of the call.
 */

#ifndef XAGENT_SESSION_H
#define XAGENT_SESSION_H

#include <stddef.h>
#include <sys/types.h>
#include <xagent/agent.h>
#include <xagent/message.h>
#include <xagent/query.h>
#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Opaque handle to a session instance.
 */
XDEF_HANDLE(xAgentSession);

/**
 * @brief Who initiated the conversation this session is about to run.
 *
 * The session is an opaque conduit for tokens and tool calls; the
 * xai core itself treats every input identically. The origin field
 * is recorded so that higher layers (a future Agent layer in
 * particular) can branch on "was this a real user talking, or an
 * internally-synthesised nudge?" without sniffing the history.
 *
 * Concretely, a long-term memory extractor that runs on the L1
 * candidates coming out of a session must NOT fold content from a
 * @ref xAgentInputOrigin_SystemSynthesized run into user preferences.
 * Agent-initiated "main character reminders" / proactive wake-ups
 * take this path.
 *
 * Callers that don't care leave the field zero — which maps to
 * @ref xAgentInputOrigin_User, the conservative default for
 * human-driven conversations.
 */
XDEF_ENUM(xAgentInputOrigin){
  xAgentInputOrigin_User = 0, /**< Real user speaking (default)    */
  xAgentInputOrigin_SystemSynthesized =
    1, /**< Agent-composed wake-up / nudge  */
};

/**
 * @brief Why the session's current run stopped.
 *
 * Defined in <xagent/query.h>. That header is included by this one
 * so the type is always available to session.h consumers.
 */

/**
 * @brief Streaming callbacks delivered to the caller.
 *
 * All callbacks are optional except @ref on_done; pass NULL for the
 * ones you don't care about. Any pointers passed into callbacks are
 * only valid for the duration of the call – copy anything you need
 * to retain. All callbacks run on the agent's event loop thread.
 */
/**
 * @brief Sidecar query lifecycle events, delivered via
 *        xAgentSessionCallbacks::on_sidecar.
 *
 * The sidecar is a lightweight query launched when the main query's
 * async tool (e.g. shell) has gone idle. These events let the caller
 * show sidecar status in the UI.
 */
XDEF_ENUM(xAgentSidecarEvent){
  xAgentSidecarEvent_Started = 0, /**< Sidecar query just launched.   */
  xAgentSidecarEvent_Done    = 1, /**< Sidecar query completed.       */
};

XDEF_STRUCT(xAgentSessionCallbacks) {
  /**
   * @brief Fired for each streamed assistant text chunk.
   *
   * @param sess   The session producing the token stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_text)(xAgentSession sess, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired for each streamed chain-of-thought ("thinking") chunk.
   *
   * Only reasoning-capable models (kimi-k2.6 thinking, DeepSeek-R1,
   * o1, ...) emit these on the `delta.reasoning_content` channel.
   * Non-thinking models never fire this callback at all. Leave NULL
   * if the caller doesn't want to surface the model's reasoning —
   * the session still records it internally so the next tool-loop
   * round can echo it back (some servers require it), it just won't
   * be streamed out.
   *
   * Fires strictly before on_text within a given assistant turn.
   *
   * @param sess   The session producing the thinking stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_thinking)(xAgentSession sess, const char *chunk, size_t len,
                      void *ud);

  /**
   * @brief Fired exactly once when the current run terminates.
   *
   * Every successfully-accepted xAgentSessionInput() call produces
   * exactly one on_done. Failure cases may additionally fire
   * on_error first as a diagnostic precursor (see on_error below),
   * but on_done is always the authoritative terminator — callers
   * that only want to react to "the run is over" can wire just
   * this callback.
   *
   * @param sess    The session.
   * @param reason  Coarse completion reason. Failure modes map to
   *                xAgentDoneReason_ModelError / _ToolError; in those
   *                cases on_error has already fired with the
   *                specific xErrno.
   * @param usage   Token usage for this run. prompt_tokens is the
   *                maximum across all provider rounds (each round
   *                reports the full input size it saw, so the last
   *                round's value represents the total input).
   *                completion_tokens and total_tokens are cumulative
   *                (additive) across rounds. NULL if the provider
   *                never reported any. Fields that are still unknown
   *                use -1 as a sentinel. Pointer is valid only for
   *                the duration of the callback — copy what you want
   *                to keep.
   * @param ud      The user_data pointer from this struct.
   */
  void (*on_done)(xAgentSession sess, xAgentDoneReason reason,
                  const xAgentUsage *usage, void *ud);

  /**
   * @brief Fired as a diagnostic precursor when the run hits a
   *        non-recoverable error.
   *
   * on_error is NOT a terminator — it is always followed by an
   * on_done with reason == xAgentDoneReason_ModelError or
   * xAgentDoneReason_ToolError in the same event-loop iteration. Its
   * sole purpose is to surface the specific @p err / @p msg pair
   * that on_done's coarse reason does not carry.
   *
   * Callers that only care about "did the run succeed" can ignore
   * on_error entirely and check on_done's reason. Callers that
   * want the error's detail wire on_error too and remember it
   * until on_done fires.
   *
   * Recoverable failures (unknown tool, handler returning non-Ok)
   * do NOT fire on_error — they are folded back to the model as
   * is_error=1 tool_result and the loop continues.
   *
   * @param sess  The session.
   * @param err   xErrno describing the failure class.
   * @param msg   Human-readable diagnostic (may be NULL).
   * @param ud    The user_data pointer from this struct.
   */
  void (*on_error)(xAgentSession sess, xErrno err, const char *msg, void *ud);

  /**
   * @brief Optional: coarse tool-execution progress signal.
   *
   * Delivered twice per tool invocation: once with @p started == 1
   * when the tool handler is about to run, and once with
   * @p started == 0 when it has finished. The tool's input and
   * output are intentionally not exposed; this callback exists for
   * UI affordances (spinners, status lines) only.
   *
   * @param sess       The session.
   * @param tool_name  The registered tool name.
   * @param started    Non-zero = started; zero = finished.
   * @param ud         The user_data pointer from this struct.
   */
  void (*on_tool)(xAgentSession sess, const char *tool_name, int started,
                  void *ud);

  /**
   * @brief Optional: streaming output from an in-flight tool.
   *
   * Fired by async tools (e.g. shell) to deliver incremental output
   * before the final tool_result. May be NULL = caller does not want
   * streaming output.
   *
   * @param sess        The session.
   * @param tool_use_id The tool_use_id of the in-flight tool call.
   * @param tool_name   The registered tool name.
   * @param data        Output chunk (NOT NUL-terminated).
   * @param len         Length of @p data in bytes.
   * @param ud          The user_data pointer from this struct.
   */
  void (*on_tool_output)(xAgentSession sess, const char *tool_use_id,
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
   * call xAgentToolConfirmResolve() with the decision — either
   * synchronously (from inside this callback) or deferred (typical
   * case: the host stashes the resolver and calls Resolve later from
   * a keypress handler). While the resolver is outstanding the
   * owning query is paused: the tool handler is not dispatched,
   * on_tool(started=1) has not yet fired, and no follow-up provider
   * round is issued. Other tools in the same batch that do NOT need
   * confirmation are dispatched normally in parallel.
   *
   * Multiple needs_confirm tool calls arriving in the same assistant
   * turn trigger on_tool_confirm once per call, each with its own
   * independent resolver. The host may decide each one independently
   * (allow some, reject others) or aggregate the UI however it
   * likes.
   *
   * If this callback is NULL, all @c needs_confirm tools run without
   * asking — the gate is disabled by default so existing callers
   * see no behaviour change.
   *
   * Cancellation: if the owning run is cancelled (via
   * xAgentSessionCancel or xAgentSessionDestroy) while a resolver is
   * outstanding, the pending call is synthesised as a cancelled
   * tool_result and the resolver becomes a silent no-op.
   *
   * @param sess         The session.
   * @param tool_name    The registered tool name.
   * @param tool_use_id  The provider-supplied tool_use_id for this call.
   * @param args_json    The JSON argument string the model supplied
   *                     (valid only for the duration of the call —
   *                     deep-copy if needed beyond the UI turn).
   * @param resolver     Opaque handle; pass to xAgentToolConfirmResolve.
   * @param ud           The user_data pointer from this struct.
   */
  void (*on_tool_confirm)(xAgentSession sess, const char *tool_name,
                          const char *tool_use_id, const char *args_json,
                          xAgentToolConfirmResolver resolver, void *ud);

  /**
   *
   * Fired when the session launches or finishes a sidecar query.
   * The sidecar is a lightweight, internally-triggered query that
   * runs alongside the main query when an async tool has gone idle
   * (no output for sidecar_idle_ms). It lets the AI inspect the
   * situation and decide whether to send input (via shell_stdin),
   * cancel, or take no action.
   *
   * Between xAgentSidecarEvent_Started and _Done, any on_text
   * callbacks originate from the sidecar query (not the main one),
   * so the UI can label them accordingly. May be NULL = caller
   * does not care about sidecar events.
   *
   * @param sess   The session.
   * @param event  The sidecar lifecycle event.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_sidecar)(xAgentSession sess, xAgentSidecarEvent event, void *ud);

  /** Forwarded to every callback in this struct. */
  void *user_data;
};

/**
 * @brief Late-teardown hook fired exactly once when a session is
 *        destroyed.
 *
 * Runs from xAgentSessionDestroy @em before any session-owned storage
 * (history, query buffers) is released. The intended consumer is a
 * future Agent layer that needs a final chance to digest the
 * session (extract L1 memory candidates, record mood delta, emit
 * analytics) with the full session state still intact. It is not
 * meant for end-user callers — regular completion flows through
 * xAgentSessionCallbacks::on_done.
 *
 * Semantics:
 *   - Fires exactly once per successfully-created session, during
 *     xAgentSessionDestroy, while @p sess is still fully live. The
 *     handle MUST NOT be stored past the callback.
 *   - If the session was still running when destroy was invoked,
 *     cancellation is issued first and on_done has already fired
 *     before this hook runs.
 *   - NULL = no hook, which is the default.
 *
 * @param sess   The session about to be torn down.
 * @param owner  The xAgentSessionConf::finalizing_owner pointer.
 */
typedef void (*xAgentSessionFinalizingFunc)(xAgentSession sess, void *owner);

/**
 * @brief Kind of a session history entry.
 *
 * Each history entry carries exactly one content block; consecutive
 * Assistant-role entries are folded into a single xAgentMessage at
 * submit time by the view builder.
 *
 * The values match the internal xAgentSessionEntryKind_ enumerators
 * so that a simple cast suffices; however, the public enum is the
 * stable ABI and the internal one may evolve independently.
 */
XDEF_ENUM(xAgentSessionEntryKind){
  xAgentSessionEntryKind_Text       = 0, /**< role + text payload      */
  xAgentSessionEntryKind_ToolUse    = 1, /**< role==Assistant + tool   */
  xAgentSessionEntryKind_ToolResult = 2, /**< role==Tool + result      */
  xAgentSessionEntryKind_Thinking   = 3, /**< role==Assistant + CoT    */
};

/**
 * @brief One entry in the session's rolling history.
 *
 * The layout is a public, read-only mirror of the internal
 * xAgentSessionMsg_ struct.  All string fields point into session-
 * owned storage and are only valid for the duration of the
 * callback that delivers them; consumers must deep-copy anything
 * they want to retain.
 *
 * Which fields are populated depends on @ref kind:
 *   - Text / Thinking: @c text, @c text_len
 *   - ToolUse: @c tool_use_id, @c tool_use_name, @c tool_use_args
 *   - ToolResult: @c tool_result_id, @c tool_result_output,
 *     @c tool_result_output_len, @c tool_result_is_error
 *
 * @c created_at_ms is the wall-clock unix-millisecond timestamp
 * recording when the entry was produced (user input, assistant
 * stream, tool completion). Zero means "unknown" — typically that
 * the entry came from a path that does not stamp, such as a
 * handful of legacy tests or memory backends that do not
 * round-trip the field. Consumers rendering a timeline should
 * treat zero as "no timestamp available" rather than as the unix
 * epoch.
 */
XDEF_STRUCT(xAgentSessionMsg) {
  xAgentRole             role;
  xAgentSessionEntryKind kind;

  /* kind == Text / Thinking */
  const char *text;
  size_t      text_len;

  /* kind == ToolUse */
  const char *tool_use_id;
  const char *tool_use_name;
  const char *tool_use_args; /* JSON object string */

  /* kind == ToolResult */
  const char *tool_result_id;
  const char *tool_result_output;
  size_t      tool_result_output_len;
  int         tool_result_is_error;

  /* Wall-clock unix-ms when this entry was produced. 0 = unknown. */
  uint64_t created_at_ms;

  /* Non-zero when this entry is a summary produced by the
   * Summarize budget policy. This field exists so the
   * public struct's layout matches the internal xAgentSessionMsg_
   * layout — without it, casting an internal array to a public
   * array would misalign every element after the first. */
  int is_summary;
};

/**
 * @brief Why the L1 preserve callback was invoked.
 *
 * The reason tells the consumer whether this is an incremental
 * slice (entries about to be trimmed/compacted) or the final
 * full delivery (session teardown).
 */
XDEF_ENUM(xAgentL1PreserveReason){
  /** TruncateTail: tail-end entries [n, end) are about to be
   *  silently dropped, preserving the prefix for prompt caching.
   *  The @p n parameter in the callback indicates the first entry
   *  being dropped; entries [0, n) survive as the cache-stable
   *  prefix. */
  xAgentL1PreserveReason_Truncated = 0,

  /** Summarize compact: entries [0, n) are about to be
   *  replaced by a summary (this is a head-trimming operation,
   *  so the [0, n) range remains correct). The consumer may want
   *  to keep the original entries for full-fidelity L1 storage
   *  even though a summary will replace them in the session's
   *  history. */
  xAgentL1PreserveReason_Compacted = 1,

  /** Session teardown: the full remaining history is being
   *  delivered as a final L1 snapshot. This fires from
   *  xAgentSessionDestroy before any session-owned storage is
   *  released. */
  xAgentL1PreserveReason_Finalizing = 2,
};

/**
 * @brief L1 memory-preservation callback, fired when the session
 *        is about to discard history entries (due to a budget
 *        policy like TruncateTail or Summarize compact).
 *
 * The callback receives a read-only slice of the entries that are
 * about to be removed from the session's rolling history. The
 * Agent layer (or any registered consumer) can deep-copy them
 * into its own L1 store before they are lost.
 *
 * Additionally, this callback fires once during xAgentSessionDestroy
 * with @p reason == @ref xAgentL1PreserveReason_Finalizing and the
 * full remaining history, so that sessions that never triggered a
 * budget event still deliver their complete conversation to L1.
 *
 * Semantics:
 *   - Entries are only valid for the duration of the callback —
 *     the caller must deep-copy anything it wants to retain.
 *   - The callback runs on the agent event loop.
 *   - NULL = no L1 preservation, which is the default.
 *
 * @param sess     The session.
 * @param msgs     Read-only array of entries about to be lost.
 * @param n_msgs   Number of entries in @p msgs.
 * @param reason   Why the entries are being preserved.
 * @param owner    The xAgentSessionConf::l1_preserve_owner pointer.
 *
 * @see xAgentL1PreserveReason — why the callback was invoked.
 */
typedef void (*xAgentSessionL1PreserveFunc)(xAgentSession           sess,
                                            const xAgentSessionMsg *msgs,
                                            size_t                  n_msgs,
                                            xAgentL1PreserveReason  reason,
                                            void                   *owner);

/**
 * @brief Strategy for keeping the serialized prompt under the
 *        session's token budget.
 *
 * Consulted by the session immediately before a new Query is
 * launched: "am I about to send more tokens to the provider than I
 * am allowed to?" The policy decides what the session does in that
 * case — drop older history, bail out with an error, summarise
 * offline, etc. All policies preserve four invariants regardless
 * of which variant is selected:
 *
 *   1. the System prompt, if any, is never trimmed;
 *   2. the current user turn being submitted is never trimmed;
 *   3. tool_use / tool_result pairs are trimmed atomically — a
 *      surviving tool_use always has its matching tool_result and
 *      vice versa;
 *   4. the most recent User turn is always preserved; older
 *      entries are compacted into a summary first.
 *
 * Zero (@ref xAgentBudgetPolicy_Disabled) means "do nothing", which
 * matches existing sessions byte-for-byte and is the default for
 * calloc'd configs.
 *
 * The recommended default is @ref xAgentBudgetPolicy_Summarize,
 * which applies the full pipeline: retroactive tool_result trimming
 * → Summarize (compress old history). If the summarise step
 * fails (empty output / OOM / provider error), the session reports
 * the failure via @ref xAgentBudgetEvent_CompactDone with
 * @c summary_ok=false and leaves the history untouched — the caller
 * decides whether to retry, relax the budget, or surface an error.
 */
XDEF_ENUM(xAgentBudgetPolicy){
  xAgentBudgetPolicy_Disabled  = 0, /**< No budget check runs      */
  xAgentBudgetPolicy_Error     = 1, /**< Fail with PromptTooLong   */
  xAgentBudgetPolicy_Summarize = 2, /**< Trim tool results, then
                                              summarise old history.
                                              On failure the session
                                              reports the error and
                                              leaves history intact.    */
};

/**
 * @brief Budget-policy lifecycle events delivered to the caller.
 *
 * These are side-channel notifications that fire alongside (not
 * instead of) the regular xAgentSessionCallbacks. They exist so the
 * caller can:
 *   - distinguish xErrno_Busy caused by an in-flight compact from
 *     Busy caused by a normal user Query still running;
 *   - learn when a compact finishes (the session auto-retries the
 *     pending input, but the caller may want to update its UI);
 *   - learn the token breakdown when a gate check passes.
 *
 * All events are informational — ignoring them does not change the
 * session's behaviour. The callback runs on the agent event loop.
 *
 * @see xAgentBudgetEventFunc — the callback signature.
 * @see xAgentBudgetConf::on_budget_event — where to register it.
 */
XDEF_ENUM(xAgentBudgetEvent){
  /** Summarize: an internal compact Query has been launched
   *  to compress old history. The session is now Busy until the
   *  compact finishes (xAgentBudgetEvent_CompactDone). The caller's
   *  xAgentSessionInput has returned xErrno_Busy. */
  xAgentBudgetEvent_Compacting = 0,

  /** Summarize: the compact Query has finished. @p info
   *  carries a xAgentBudgetCompactDoneInfo with the result. The
   *  session will auto-retry the pending input; the caller does
   *  not need to re-submit. */
  xAgentBudgetEvent_CompactDone = 1,

  /** GatePassed: the budget gate allowed the incoming message
   *  through — history + incoming fit within the limit. @p info
   *  carries a xAgentBudgetGateInfo with the token breakdown so
   *  the caller can display remaining context capacity. Fires
   *  once per successful xAgentSessionInput, before the Query is
   *  submitted. */
  xAgentBudgetEvent_GatePassed = 2,
};

/**
 * @brief Extra detail passed with xAgentBudgetEvent_Compacting.
 */
XDEF_STRUCT(xAgentBudgetCompactInfo) {
  /** Number of history entries being summarised (entries [0, n)
   *  are the ones being compacted). */
  size_t entries_compacted;
};

/**
 * @brief Extra detail passed with xAgentBudgetEvent_CompactDone.
 */
XDEF_STRUCT(xAgentBudgetCompactDoneInfo) {
  /** Non-zero if the compact produced a usable summary; zero if
   *  the summarise step failed (empty output, OOM, or provider
   *  error). When zero the session leaves history untouched and
   *  the caller should decide whether to retry, relax the budget,
   *  or surface an error to the end user. */
  int summary_ok;

  /** Token count of the new summary entry (0 if summary_ok == 0
   *  or the estimator was not run on the result). */
  size_t summary_tokens;

  /** Number of original entries that were replaced (summary_ok != 0)
   *  or that the compact attempted to cover (summary_ok == 0). When
   *  summary_ok == 0, history is unchanged regardless of this value. */
  size_t entries_affected;
};

/**
 * @brief Extra detail passed with xAgentBudgetEvent_GatePassed.
 */
XDEF_STRUCT(xAgentBudgetGateInfo) {
  /** The effective token ceiling (from budget.context_window or the
   *  built-in default). */
  size_t limit;

  /** Token count used for the gate decision. This is either:
   *   - The provider-reported prompt_tokens from the last completed
   *     run (known_prompt_tokens) + estimated delta for new entries,
   *     when a provider report is available; or
   *   - The coarse estimate (bytes/4 + envelope) of the full
   *     history + incoming message when no provider report exists
   *     yet (cold start or post-trim invalidation). */
  size_t estimated;

  /** Remaining budget: limit - estimated. */
  size_t remaining;

  /** The provider-reported prompt_tokens from the FIRST round of
   *  the PREVIOUS run, or -1 if not available. This is the precise
   *  baseline used for incremental bookkeeping. Useful for callers
   *  to display "actual tokens used" without the inflation that
   *  later tool-loop rounds would introduce. */
  int last_first_round_prompt_tokens;
};

/**
 * @brief Callback invoked when a budget-policy lifecycle event fires.
 *
 * @p info is event-specific; its concrete type depends on @p event:
 *   - Compacting  → xAgentBudgetCompactInfo
 *   - CompactDone → xAgentBudgetCompactDoneInfo
 *   - GatePassed  → xAgentBudgetGateInfo
 *
 * @p info may be NULL if the implementation cannot provide detail
 * (e.g. OOM while building the struct). The caller must check.
 *
 * @param sess   The session.
 * @param event  Which budget event fired.
 * @param info   Event-specific detail (may be NULL).
 * @param ud     The user_data from xAgentBudgetConf.
 */
typedef void (*xAgentBudgetEventFunc)(xAgentSession     sess,
                                      xAgentBudgetEvent event, const void *info,
                                      void *ud);

/**
 * @brief Configuration for the session's context-budget enforcement.
 *
 * All fields are optional. When @ref policy is
 * @ref xAgentBudgetPolicy_Disabled the other two are ignored, and the
 * session behaves exactly as if this struct did not exist — this
 * is the default for zero-initialised configs, so existing callers
 * need not change a single line.
 *
 * The budget is expressed in **approximate tokens**, not bytes.
 * The session uses incremental bookkeeping: provider-reported
 * prompt_tokens serve as the precise baseline, and only the delta
 * (new entries since the last provider report) is estimated with
 * the coarse bytes/4 heuristic. This gives much higher accuracy
 * than re-estimating the full history every turn.
 */
XDEF_STRUCT(xAgentBudgetConf) {
  /**
   * @brief Which policy the session applies when the estimated
   *        prompt size is about to exceed @ref context_window.
   *
   * @ref xAgentBudgetPolicy_Disabled (the zero default) disables the
   * check entirely; the other fields are then ignored.
   */
  xAgentBudgetPolicy policy;

  /**
   * @brief Model context window in tokens.
   *
   * The session treats this as the hard ceiling for the serialized
   * prompt (system + history + incoming user message). When the
   * estimated size is about to exceed it, the enforcement policy
   * (@ref policy) kicks in.
   *
   * Zero means "let the session fall back to its built-in default
   * for the active model" — the concrete default is
   * implementation-defined and may change across releases. Callers
   * with a hard downstream limit should set this explicitly.
   *
   * Naming note: this field was previously called @c max_tokens,
   * which collided with the per-round completion cap of the same
   * name on @ref xAgentSessionConf. Renamed to @c context_window
   * to reflect what it actually models.
   */
  size_t context_window;

  /**
   * @brief Optional callback for budget-policy lifecycle events.
   *
   * Fires when the budget gate takes observable action (compacting
   * old history, completing a compact). Leave NULL if you don't
   * need these notifications — the session's behaviour is the same
   * either way.
   *
   * This is a side-channel: it does NOT replace on_done or any
   * xAgentSessionCallbacks entry. It exists so callers can
   * distinguish "Busy because compacting" from "Busy because a
   * user Query is in flight" and know when to update their UI.
   */
  xAgentBudgetEventFunc on_budget_event;

  /** Forwarded to every invocation of @ref on_budget_event. */
  void *budget_event_ud;

  /**
   * @brief Number of earliest user turns to preserve during compact.
   *
   * When the Summarize policy triggers a compact, the session
   * replaces history[compact_start..compact_end) with a summary.
   * compact_start is the index of the user turn at position
   * @c context_preserve_head_turns — everything before it (the
   * "head") stays intact.
   *
   * 0 = no head preserved (compact from the beginning).
   */
  size_t context_preserve_head_turns;

  /**
   * @brief Number of latest user turns to preserve during compact.
   *
   * compact_end is the index of the user turn at position
   * (user_count - @c context_preserve_tail_turns) — everything
   * from it onward (the "tail") stays intact.
   *
   * 0 = no tail preserved (compact to the end of history).
   */
  size_t context_preserve_tail_turns;
};

/**
 * @brief Configuration for creating a session.
 *
 * Zero-initialise for "inherit everything from the agent". All
 * override fields that are NULL / 0 defer to the agent's defaults.
 */
XDEF_STRUCT(xAgentSessionConf) {
  xAgentSessionCallbacks cbs; /**< Streaming callbacks (in-place, not a
                                pointer, so the caller does not need to
                                keep a separate struct alive).          */

  const char *system_prompt; /**< Override the agent's system prompt
                                  (borrowed, may be NULL to inherit).   */
  const char *model;         /**< Override the agent's default model
                                  id (borrowed, may be NULL).           */

  int max_turns;         /**< Override the agent's max_turns
                              (0 = inherit).                        */
  int max_tokens;        /**< Override per-round token cap
                              (0 = inherit).                        */
  size_t context_budget; /**< DEPRECATED: use @ref budget instead.
                              This field is no longer inherited from
                              the agent and is ignored by the session
                              when @ref budget is configured. Retained
                              for ABI compatibility only; will be
                              removed in a future release.             */
  /**
   * @brief Structured context-budget policy for this session.
   *
   * Zero-initialised (@ref xAgentBudgetPolicy_Disabled) behaves
   * exactly like previous releases: the session never trims and
   * never refuses a turn on prompt size. Setting a non-Disabled
   * policy opts the session into the enforcement path described
   * in xAgentBudgetPolicy / xAgentBudgetConf. Unlike @ref context_budget,
   * this field is **not** inherited from the agent today — the
   * session is the only layer that owns the rolling history and
   * therefore the only layer that can trim it. Agent-level
   * inheritance can be added non-breakingly later.
   */
  xAgentBudgetConf budget;

  /**
   * @brief Who this session speaks for.
   *
   * Stamped on the session at creation and never changes afterwards
   * — it is a property of "this conversation", not of individual
   * inputs. Readable via xAgentSessionOrigin(); inspected by future
   * Agent-layer hooks that need to discriminate user-driven runs
   * from system-synthesised ones (L1 memory extraction, mood
   * accounting). Zero defaults to xAgentInputOrigin_User.
   */
  xAgentInputOrigin origin;

  /**
   * @brief Optional late-teardown hook, paired with @ref finalizing_owner.
   *
   * See xAgentSessionFinalizingFunc for semantics. Leave NULL if not
   * used. Regular application code does NOT need this — it exists
   * so the future Agent layer can attach without a second round of
   * API changes.
   */
  xAgentSessionFinalizingFunc on_finalizing;
  void                       *finalizing_owner; /**< Passed back to
                                                     @ref on_finalizing. */

  /**
   * @brief Optional L1 memory-preservation callback, paired with
   *        @ref l1_preserve_owner.
   *
   * Fires when the session is about to discard history entries
   * (TruncateTail / Summarize compact), and once at
   * teardown with the full remaining history. The Agent layer
   * uses this to capture the complete conversation before any
   * information is lost. Leave NULL if not used.
   *
   * @see xAgentSessionL1PreserveFunc for the full callback contract.
   */
  xAgentSessionL1PreserveFunc on_l1_preserve;
  void                       *l1_preserve_owner; /**< Passed back to
                                                      @ref on_l1_preserve. */

  /**
   * @brief Unique identifier for this session instance.
   *
   * Used by the agent's pluggable memory store (see
   * xAgentConf::memory) to key per-session state. The built-in
   * JSONL backend, for example, lays out one file per id under
   *   {root_dir}/sessions/{session_id}/memory.jsonl
   * Borrowed from the caller; must remain alive for the session's
   * lifetime. When NULL the agent may auto-generate one during
   * xAgentCreateSession(). Readable via xAgentSessionId().
   */
  const char *session_id;

  /**
   * @brief Idle timeout (in ms) before launching a sidecar Query
   *        when an async tool call has not produced output.
   *
   * When the main Query is blocked waiting for an async tool (e.g.
   * a long-running shell command) and no streaming output has been
   * received for this many milliseconds, the Session automatically
   * launches a lightweight sidecar Query. The sidecar is given the
   * accumulated tool output and a restricted tool set (e.g.
   * shell_stdin) so the AI can decide whether to send input, cancel,
   * or take other diagnostic action.
   *
   * Zero (the default) disables the sidecar mechanism entirely —
   * the Session never launches one. This is the safe default for
   * sessions that do not use interactive async tools.
   *
   * Typical values: 5000–15000 ms depending on expected command
   * latency. Shorter values make the AI respond faster to prompts,
   * at the cost of more LLM invocations for commands that are
   * genuinely slow but don't need input.
   */
  uint64_t sidecar_idle_ms;
};

/**
 * @brief Create a session bound to the given agent.
 *
 * The session borrows @p agent for its lifetime; the caller must
 * keep @p agent alive until xAgentSessionDestroy() has been called.
 * The session captures @p conf by value (including the callbacks
 * struct), so @p conf may be stack-allocated.
 *
 * @param agent  Agent to derive the session from (must not be NULL).
 * @param conf   Session configuration (must not be NULL;
 *               conf->cbs.on_done should usually be set).
 * @return       A new session handle, or NULL on failure.
 */
XCAPI(xAgentSession)
xAgentSessionCreate(xAgent agent, const xAgentSessionConf *conf);

/**
 * @brief Feed a new user message into the session.
 *
 * Kicks off (or continues) the agent loop: the message is appended
 * to the history, the provider is invoked, and streaming callbacks
 * begin firing. Only one run may be active per session at a time;
 * calling this while a previous run is still in flight returns
 * xErrno_Busy. Each accepted call is terminated by exactly one
 * on_done callback; on_error may additionally precede it with
 * diagnostic detail on failure paths.
 *
 * The message is shallow-copied: all string pointers inside @p msg
 * (and inside its content blocks) must remain valid until the
 * surrounding run terminates. For the common "send one text line"
 * case, see xAgentMessageFromText().
 *
 * @param sess  Session handle (must not be NULL).
 * @param msg   Message to append and run on (role is usually User).
 * @return      xErrno_Ok on acceptance; xErrno_Busy if a run is
 *              already active; other xErrno on invalid input.
 */
XCAPI(xErrno) xAgentSessionInput(xAgentSession sess, xAgentMessage msg);

/**
 * @brief Cancel the currently active run, if any.
 *
 * Requests the provider to stop streaming and any in-flight tool
 * handlers to bail out. The on_done callback is still delivered
 * (with reason == xAgentDoneReason_Aborted) once unwinding completes.
 * Calling this while no run is active is a no-op.
 *
 * @param sess  Session handle (NULL is a no-op).
 */
XCAPI(void) xAgentSessionCancel(xAgentSession sess);

/**
 * @brief Override the provider used for the session's NEXT query.
 *
 * When @p provider is non-NULL, subsequent xAgentSessionInput() calls
 * (and any sidecar Queries they spawn) dispatch to @p provider instead
 * of the agent's default provider. When @p provider is NULL, the
 * override is cleared and the session reverts to the agent's default.
 *
 * Only affects the next Query created after this call — any Query
 * currently in flight keeps streaming on the provider it was launched
 * with. Internal maintenance Queries (e.g. history summarisation for
 * budget compaction) always use the agent's default provider and are
 * NOT affected by this override.
 *
 * Typical use: a host (e.g. a REPL) implements a "/model" command by
 * creating one provider per backend at startup and flipping the
 * active one via this setter, without tearing down the session.
 *
 * The session borrows @p provider; the caller must keep it alive
 * until either the override is cleared / replaced or the session
 * is destroyed.
 *
 * @param sess      Session handle (NULL is a no-op).
 * @param provider  Provider to use for the next query, or NULL to
 *                  clear the override.
 */
XCAPI(void) xAgentSessionSetProvider(xAgentSession  sess,
                                     xAgentProvider provider);

/**
 * @brief Switch the session to a spec registered in its agent's
 *        model registry.
 *
 * Looks up @p model_id in the agent's xAgentConf::model_registry
 * and, on success, sets the session's per-session provider and
 * model overrides to the spec's (provider, model). The next Query
 * the session launches (main or sidecar) will use the new backend;
 * any Query already in flight keeps streaming on the provider it
 * was launched with. Internal maintenance Queries (summary/compact)
 * always use the agent's default provider regardless of this
 * override.
 *
 * Passing @p model_id == NULL clears both overrides and reverts the
 * session to the agent defaults (equivalent to
 * xAgentSessionSetProvider(sess, NULL) for the provider side, while
 * also dropping any model-name override).
 *
 * Requires the agent to have been created via the REGISTRY path
 * (xAgentConf::model_registry non-NULL). Calling this on a session
 * whose agent uses the LEGACY single-provider path returns
 * xErrno_InvalidState.
 *
 * @param sess      Session handle.
 * @param model_id  Registered spec id, or NULL to clear the override.
 * @return          xErrno_Ok on success; xErrno_InvalidArg when
 *                  @p sess is NULL; xErrno_InvalidState when the
 *                  agent has no model registry; xErrno_NotFound
 *                  when @p model_id is non-NULL but not present
 *                  in the registry; xErrno_NoMemory on allocation
 *                  failure.
 */
XCAPI(xErrno) xAgentSessionSetModel(xAgentSession sess, const char *model_id);

/**
 * @brief Update the session's context-window budget in tokens.
 *
 * Overwrites @ref xAgentBudgetConf::context_window on the session's
 * budget state. The new limit is consulted on the NEXT
 * xAgentSessionInput — any run already in flight keeps the limit it
 * was admitted with. All other budget fields (policy,
 * on_budget_event, …) are left untouched.
 *
 * Intended for host apps that ship a per-model context-window
 * figure (e.g. loaded from a models.json entry) and want to keep
 * the session's budget gate in sync when the user switches models
 * at runtime via xAgentSessionSetModel().
 *
 * @param sess            Session handle (NULL is a no-op).
 * @param context_window  New context-window limit in tokens. Zero
 *                        means "fall back to the built-in default"
 *                        (xAgentBudgetConf documents the exact value).
 */
XCAPI(void) xAgentSessionSetContextWindow(xAgentSession sess,
                                          size_t        context_window);

/**
 * @brief Replace the session's budget thresholds in bulk.
 *
 * Like xAgentSessionSetContextWindow but for the @c context_window
 * field on @ref xAgentBudgetConf.
 * The @c policy stays whatever was configured at session-create
 * time, and the @c on_budget_event / @c budget_event_ud callback
 * pair is left untouched — host apps that dial budget knobs on a
 * runtime model switch should NOT have to re-plumb their event
 * sink.
 *
 * Zero in any threshold field carries the same "fall back to the
 * built-in default" semantics it has on initial setup; pass the
 * fully-populated conf you want to take effect on the next
 * xAgentSessionInput. Any Query already in flight keeps running
 * with the limits it was admitted under.
 *
 * Intended for host apps that load per-model budget overrides
 * from disk (e.g. a per-entry budget block in models.json) and
 * want every threshold — not just the window — to follow the
 * active backend.
 *
 * @param sess  Session handle (NULL is a no-op).
 * @param conf  Budget conf to apply (NULL is a no-op). Only the
 *              @c context_window field is read; @c policy and the
 *              callback fields are ignored.
 */
XCAPI(void) xAgentSessionSetBudget(xAgentSession           sess,
                                   const xAgentBudgetConf *conf);

/**
 * @brief Destroy the session and release its resources.
 *
 * Implicitly cancels any active run and drains pending callbacks.
 * After this call the session handle is invalid.
 *
 * If xAgentSessionConf::on_finalizing was set at creation time, it is
 * invoked exactly once during teardown, while the session is still
 * fully live and its history is still intact.
 *
 * @param sess  Session handle (NULL is a no-op).
 */
XCAPI(void) xAgentSessionDestroy(xAgentSession sess);

/**
 * @brief Read back the session's input origin (see xAgentInputOrigin).
 *
 * Stamped once at xAgentSessionCreate time; never changes afterwards.
 * Returns xAgentInputOrigin_User for NULL or zero-origin sessions so
 * callers can treat it as a safe conservative default.
 *
 * @param sess  Session handle.
 * @return      The origin recorded at creation, or
 *              xAgentInputOrigin_User for NULL input.
 */
XCAPI(xAgentInputOrigin) xAgentSessionOrigin(xAgentSession sess);

/**
 * @brief Read back the session's unique identifier.
 *
 * Stamped once at xAgentSessionCreate time; never changes afterwards.
 * May be NULL if no session_id was provided and the agent did not
 * auto-generate one.
 *
 * @param sess  Session handle.
 * @return      The session id string, or NULL.
 */
XCAPI(const char *) xAgentSessionId(xAgentSession sess);

/**
 * @brief Get the Query currently running on a Session, if any.
 *
 * Returns the live Query handle the Session has allocated for its
 * in-flight run, or NULL if the Session is idle. The handle stays
 * valid until either on_done has been delivered (at which point
 * the Session will destroy it) or xAgentSessionDestroy() is called.
 *
 * @param sess  Session handle (NULL returns NULL).
 * @return      The Session's current Query, or NULL if none.
 */
XCAPI(xAgentQuery) xAgentSessionQuery(xAgentSession sess);

#endif /* XAGENT_SESSION_H */
