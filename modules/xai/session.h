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
  void (*on_thinking)(xAiSession sess, const char *chunk, size_t len,
                      void *ud);

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
  void (*on_done)(xAiSession sess, xAiDoneReason reason,
                  const xAiUsage *usage, void *ud);

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

  int    max_turns;          /**< Override the agent's max_turns
                                  (0 = inherit).                        */
  int    max_tokens;         /**< Override per-round token cap
                                  (0 = inherit).                        */
  size_t context_budget;     /**< Override the agent's context budget
                                  (0 = inherit).                        */
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
 * @param sess  Session handle (NULL is a no-op).
 */
XCAPI(void) xAiSessionDestroy(xAiSession sess);

#endif /* XAI_SESSION_H */
