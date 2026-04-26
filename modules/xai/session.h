/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session.h - One conversation instance for the xai agent core
 *
 * An xAiSession is a single live conversation derived from an agent.
 * It owns the rolling message history, drives the agent loop
 * (prompt → stream → maybe tool calls → next round), and surfaces
 * streaming events to the caller through a small set of callbacks.
 *
 * Relationship to xAiAgent:
 * - The agent is a long-lived capability template (provider, tools,
 *   model defaults, limits).
 * - A session is short-lived and specific to one chat. One agent
 *   may spawn many concurrent sessions on the same event loop.
 * - Fields of xAiSessionConf override the agent's defaults for this
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
 * - Every xAiSession* call must happen on the session's agent event
 *   loop. Callbacks are invoked on that same loop. Pointers passed
 *   into callbacks are valid only for the duration of the call.
 */

#ifndef XAI_SESSION_H
#define XAI_SESSION_H

#include <stddef.h>
#include <xai/agent.h>
#include <xai/message.h>
#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Opaque handle to a session instance.
 */
XDEF_HANDLE(xAiSession);

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
 * @ref xAiInputOrigin_SystemSynthesized run into user preferences.
 * Agent-initiated "main character reminders" / proactive wake-ups
 * take this path.
 *
 * Callers that don't care leave the field zero — which maps to
 * @ref xAiInputOrigin_User, the conservative default for
 * human-driven conversations.
 */
XDEF_ENUM(xAiInputOrigin){
  xAiInputOrigin_User              = 0, /**< Real user speaking (default)    */
  xAiInputOrigin_SystemSynthesized = 1, /**< Agent-composed wake-up / nudge  */
};

/**
 * @brief Why the session's current run stopped.
 *
 * Delivered to the caller via xAiSessionCallbacks::on_done. This is
 * the coarse, caller-facing signal; the session layer internally
 * maintains a finer-grained state machine that is not exposed.
 */
XDEF_ENUM(xAiDoneReason){
  xAiDoneReason_Completed     = 0, /**< Run finished naturally        */
  xAiDoneReason_MaxTurns      = 1, /**< Agent's max_turns reached     */
  xAiDoneReason_PromptTooLong = 2, /**< Context budget exhausted      */
  xAiDoneReason_Aborted       = 3, /**< xAiSessionCancel() was called */
  xAiDoneReason_ModelError    = 4, /**< Provider/model returned error */
  xAiDoneReason_ToolError     = 5, /**< Tool handler returned error   */
  xAiDoneReason_Stopped       = 6, /**< Stopped by internal policy    */
};

/**
 * @brief Streaming callbacks delivered to the caller.
 *
 * All callbacks are optional except @ref on_done; pass NULL for the
 * ones you don't care about. Any pointers passed into callbacks are
 * only valid for the duration of the call – copy anything you need
 * to retain. All callbacks run on the agent's event loop thread.
 */
XDEF_STRUCT(xAiSessionCallbacks) {
  /**
   * @brief Fired for each streamed assistant text chunk.
   *
   * @param sess   The session producing the token stream.
   * @param chunk  Byte buffer (NOT NUL-terminated).
   * @param len    Length of @p chunk in bytes.
   * @param ud     The user_data pointer from this struct.
   */
  void (*on_text)(xAiSession sess, const char *chunk, size_t len, void *ud);

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
  void (*on_thinking)(xAiSession sess, const char *chunk, size_t len, void *ud);

  /**
   * @brief Fired exactly once when the current run terminates.
   *
   * Every successfully-accepted xAiSessionInput() call produces
   * exactly one on_done. Failure cases may additionally fire
   * on_error first as a diagnostic precursor (see on_error below),
   * but on_done is always the authoritative terminator — callers
   * that only want to react to "the run is over" can wire just
   * this callback.
   *
   * @param sess    The session.
   * @param reason  Coarse completion reason. Failure modes map to
   *                xAiDoneReason_ModelError / _ToolError; in those
   *                cases on_error has already fired with the
   *                specific xErrno.
   * @param usage   Cumulative token usage across every provider
   *                round in this run (the tool loop may submit
   *                several rounds; this is the running sum), or
   *                NULL if the provider never reported any. Fields
   *                that are still unknown use -1 as a sentinel.
   *                Pointer is valid only for the duration of the
   *                callback — copy what you want to keep.
   * @param ud      The user_data pointer from this struct.
   */
  void (*on_done)(xAiSession sess, xAiDoneReason reason, const xAiUsage *usage,
                  void *ud);

  /**
   * @brief Fired as a diagnostic precursor when the run hits a
   *        non-recoverable error.
   *
   * on_error is NOT a terminator — it is always followed by an
   * on_done with reason == xAiDoneReason_ModelError or
   * xAiDoneReason_ToolError in the same event-loop iteration. Its
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
  void (*on_error)(xAiSession sess, xErrno err, const char *msg, void *ud);

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
  void (*on_tool)(xAiSession sess, const char *tool_name, int started,
                  void *ud);

  /** Forwarded to every callback in this struct. */
  void *user_data;
};

/**
 * @brief Late-teardown hook fired exactly once when a session is
 *        destroyed.
 *
 * Runs from xAiSessionDestroy @em before any session-owned storage
 * (history, query buffers) is released. The intended consumer is a
 * future Agent layer that needs a final chance to digest the
 * session (extract L1 memory candidates, record mood delta, emit
 * analytics) with the full session state still intact. It is not
 * meant for end-user callers — regular completion flows through
 * xAiSessionCallbacks::on_done.
 *
 * Semantics:
 *   - Fires exactly once per successfully-created session, during
 *     xAiSessionDestroy, while @p sess is still fully live. The
 *     handle MUST NOT be stored past the callback.
 *   - If the session was still running when destroy was invoked,
 *     cancellation is issued first and on_done has already fired
 *     before this hook runs.
 *   - NULL = no hook, which is the default.
 *
 * @param sess   The session about to be torn down.
 * @param owner  The xAiSessionConf::finalizing_owner pointer.
 */
typedef void (*xAiSessionFinalizingFunc)(xAiSession sess, void *owner);

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
 *   4. at least the last @ref xAiBudgetConf::keep_recent_turns
 *      user turns (with their assistant replies and tool chatter)
 *      are kept.
 *
 * Zero (@ref xAiBudgetPolicy_Disabled) means "do nothing", which
 * matches existing sessions byte-for-byte and is the default for
 * calloc'd configs.
 *
 * @see docs/todo/xai_architecture.md and modules/xai/TODO.md §6
 *      for the rollout plan; alpha ships with Disabled / Error /
 *      TruncateOldest only. Callback and SummarizeOldest slots are
 *      wired into the enum early so that adding them later is not
 *      an ABI break.
 */
XDEF_ENUM(xAiBudgetPolicy){
  xAiBudgetPolicy_Disabled        = 0, /**< No budget check runs      */
  xAiBudgetPolicy_Error           = 1, /**< Fail with PromptTooLong   */
  xAiBudgetPolicy_TruncateOldest  = 2, /**< Drop oldest non-pinned    */
  xAiBudgetPolicy_Callback        = 3, /**< Reserved: caller-supplied */
  xAiBudgetPolicy_SummarizeOldest = 4, /**< Compress old history     */
  xAiBudgetPolicy_Auto             = 5, /**< Auto: dynamic policy picker */
};

/**
 * @brief Budget-policy lifecycle events delivered to the caller.
 *
 * These are side-channel notifications that fire alongside (not
 * instead of) the regular xAiSessionCallbacks. They exist so the
 * caller can:
 *   - distinguish xErrno_Busy caused by an in-flight compact from
 *     Busy caused by a normal user Query still running;
 *   - learn when a compact finishes so it can retry the input;
 *   - learn when TruncateOldest silently drops history so it can
 *     update UI or log the event.
 *
 * All events are informational — ignoring them does not change the
 * session's behaviour. The callback runs on the agent event loop.
 *
 * @see xAiBudgetEventFunc — the callback signature.
 * @see xAiBudgetConf::on_budget_event — where to register it.
 */
XDEF_ENUM(xAiBudgetEvent){
  /** SummarizeOldest: an internal compact Query has been launched
   *  to compress old history. The session is now Busy until the
   *  compact finishes (xAiBudgetEvent_CompactDone). The caller's
   *  xAiSessionInput has returned xErrno_Busy. */
  xAiBudgetEvent_Compacting  = 0,

  /** SummarizeOldest: the compact Query has finished. @p info
   *  carries a xAiBudgetCompactInfo with the result. The session
   *  is now idle and the caller can retry xAiSessionInput. */
  xAiBudgetEvent_CompactDone = 1,

  /** TruncateOldest: history entries were silently dropped to fit
   *  the budget. @p info carries a xAiBudgetTruncateInfo with the
   *  count of entries removed. */
  xAiBudgetEvent_Truncated   = 2,
};

/**
 * @brief Extra detail passed with xAiBudgetEvent_Compacting.
 */
XDEF_STRUCT(xAiBudgetCompactInfo) {
  /** Number of history entries being summarised (entries [0, n)
   *  are the ones being compacted). */
  size_t entries_compacted;
};

/**
 * @brief Extra detail passed with xAiBudgetEvent_CompactDone.
 */
XDEF_STRUCT(xAiBudgetCompactDoneInfo) {
  /** Non-zero if the compact produced a usable summary; zero if
   *  it degraded to TruncateOldest instead (empty summary, OOM,
   *  or provider error). When zero the caller should assume old
   *  history was truncated, not summarised. */
  int summary_ok;

  /** Token count of the new summary entry (0 if summary_ok == 0
   *  or the estimator was not run on the result). */
  size_t summary_tokens;

  /** Number of original entries that were replaced or removed. */
  size_t entries_affected;
};

/**
 * @brief Extra detail passed with xAiBudgetEvent_Truncated.
 */
XDEF_STRUCT(xAiBudgetTruncateInfo) {
  /** Number of history entries that were removed. */
  size_t entries_removed;
};

/**
 * @brief Callback invoked when a budget-policy lifecycle event fires.
 *
 * @p info is event-specific; its concrete type depends on @p event:
 *   - Compacting  → xAiBudgetCompactInfo
 *   - CompactDone → xAiBudgetCompactDoneInfo
 *   - Truncated   → xAiBudgetTruncateInfo
 *
 * @p info may be NULL if the implementation cannot provide detail
 * (e.g. OOM while building the struct). The caller must check.
 *
 * @param sess   The session.
 * @param event  Which budget event fired.
 * @param info   Event-specific detail (may be NULL).
 * @param ud     The user_data from xAiBudgetConf.
 */
typedef void (*xAiBudgetEventFunc)(xAiSession      sess,
                                  xAiBudgetEvent  event,
                                  const void     *info,
                                  void           *ud);

/**
 * @brief Configuration for the session's context-budget enforcement.
 *
 * All three fields are optional. When @ref policy is
 * @ref xAiBudgetPolicy_Disabled the other two are ignored, and the
 * session behaves exactly as if this struct did not exist — this
 * is the default for zero-initialised configs, so existing callers
 * need not change a single line.
 *
 * The budget is expressed in **approximate tokens**, not bytes.
 * The session's internal estimator is deliberately coarse
 * (bytes/4 baseline, ex-post calibrated against provider-reported
 * xAiUsage once a run reports one) and is never exposed here —
 * callers who care about precision should over-provision by ~10%.
 */
XDEF_STRUCT(xAiBudgetConf) {
  /**
   * @brief Which policy the session applies when the estimated
   *        prompt size is about to exceed @ref max_tokens.
   *
   * @ref xAiBudgetPolicy_Disabled (the zero default) disables the
   * check entirely; the other fields are then ignored.
   */
  xAiBudgetPolicy policy;

  /**
   * @brief Estimated-token ceiling for the serialized prompt.
   *
   * Zero means "let the session fall back to its built-in default
   * for the active model" — the concrete default is
   * implementation-defined and may change across releases. Callers
   * with a hard downstream limit should set this explicitly.
   */
  size_t max_tokens;

  /**
   * @brief Minimum number of recent user turns to keep intact,
   *        regardless of how much token pressure the trimmer is
   *        under.
   *
   * A "user turn" here means one xAiRole_User message plus every
   * assistant / tool entry that followed before the next user
   * message. Zero means "no minimum" — use with care, since a
   * pathological budget could otherwise leave the model with only
   * the current input and no conversational context. The session
   * may clamp very high values downward if honouring them would
   *     itself violate @ref max_tokens.
   */
  size_t keep_recent_turns;

  /**
   * @brief Optional callback for budget-policy lifecycle events.
   *
   * Fires when the budget gate takes observable action (compacting
   * old history, completing a compact, truncating entries). Leave
   * NULL if you don't need these notifications — the session's
   * behaviour is the same either way.
   *
   * This is a side-channel: it does NOT replace on_done or any
   * xAiSessionCallbacks entry. It exists so callers can
   * distinguish "Busy because compacting" from "Busy because a
   * user Query is in flight" and know when to retry.
   */
  xAiBudgetEventFunc on_budget_event;

  /** Forwarded to every invocation of @ref on_budget_event. */
  void *budget_event_ud;
};

/**
 * @brief Configuration for creating a session.
 *
 * Zero-initialise for "inherit everything from the agent". All
 * override fields that are NULL / 0 defer to the agent's defaults.
 */
XDEF_STRUCT(xAiSessionConf) {
  xAiSessionCallbacks cbs; /**< Streaming callbacks (in-place, not a
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
   * Zero-initialised (@ref xAiBudgetPolicy_Disabled) behaves
   * exactly like previous releases: the session never trims and
   * never refuses a turn on prompt size. Setting a non-Disabled
   * policy opts the session into the enforcement path described
   * in xAiBudgetPolicy / xAiBudgetConf. Unlike @ref context_budget,
   * this field is **not** inherited from the agent today — the
   * session is the only layer that owns the rolling history and
   * therefore the only layer that can trim it. Agent-level
   * inheritance can be added non-breakingly later.
   */
  xAiBudgetConf budget;

  /**
   * @brief Who this session speaks for.
   *
   * Stamped on the session at creation and never changes afterwards
   * — it is a property of "this conversation", not of individual
   * inputs. Readable via xAiSessionOrigin(); inspected by future
   * Agent-layer hooks that need to discriminate user-driven runs
   * from system-synthesised ones (L1 memory extraction, mood
   * accounting). Zero defaults to xAiInputOrigin_User.
   */
  xAiInputOrigin origin;

  /**
   * @brief Optional late-teardown hook, paired with @ref finalizing_owner.
   *
   * See xAiSessionFinalizingFunc for semantics. Leave NULL if not
   * used. Regular application code does NOT need this — it exists
   * so the future Agent layer can attach without a second round of
   * API changes.
   */
  xAiSessionFinalizingFunc on_finalizing;
  void                  *finalizing_owner; /**< Passed back to
                                                @ref on_finalizing. */
};

/**
 * @brief Create a session bound to the given agent.
 *
 * The session borrows @p agent for its lifetime; the caller must
 * keep @p agent alive until xAiSessionDestroy() has been called.
 * The session captures @p conf by value (including the callbacks
 * struct), so @p conf may be stack-allocated.
 *
 * @param agent  Agent to derive the session from (must not be NULL).
 * @param conf   Session configuration (must not be NULL;
 *               conf->cbs.on_done should usually be set).
 * @return       A new session handle, or NULL on failure.
 */
XCAPI(xAiSession)
xAiSessionCreate(xAiAgent agent, const xAiSessionConf *conf);

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
 * case, see xAiMessageFromText().
 *
 * @param sess  Session handle (must not be NULL).
 * @param msg   Message to append and run on (role is usually User).
 * @return      xErrno_Ok on acceptance; xErrno_Busy if a run is
 *              already active; other xErrno on invalid input.
 */
XCAPI(xErrno) xAiSessionInput(xAiSession sess, xAiMessage msg);

/**
 * @brief Cancel the currently active run, if any.
 *
 * Requests the provider to stop streaming and any in-flight tool
 * handlers to bail out. The on_done callback is still delivered
 * (with reason == xAiDoneReason_Aborted) once unwinding completes.
 * Calling this while no run is active is a no-op.
 *
 * @param sess  Session handle (NULL is a no-op).
 */
XCAPI(void) xAiSessionCancel(xAiSession sess);

/**
 * @brief Destroy the session and release its resources.
 *
 * Implicitly cancels any active run and drains pending callbacks.
 * After this call the session handle is invalid.
 *
 * If xAiSessionConf::on_finalizing was set at creation time, it is
 * invoked exactly once during teardown, while the session is still
 * fully live and its history is still intact.
 *
 * @param sess  Session handle (NULL is a no-op).
 */
XCAPI(void) xAiSessionDestroy(xAiSession sess);

/**
 * @brief Read back the session's input origin (see xAiInputOrigin).
 *
 * Stamped once at xAiSessionCreate time; never changes afterwards.
 * Returns xAiInputOrigin_User for NULL or zero-origin sessions so
 * callers can treat it as a safe conservative default.
 *
 * @param sess  Session handle.
 * @return      The origin recorded at creation, or
 *              xAiInputOrigin_User for NULL input.
 */
XCAPI(xAiInputOrigin) xAiSessionOrigin(xAiSession sess);

#endif /* XAI_SESSION_H */
