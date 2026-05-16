/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider.h - LLM provider abstraction for the xai agent core
 *
 * A provider turns a list of xAgentMessage + xAgentTool into a stream of
 * delta events (text chunks, emerging tool calls, completion reasons).
 * It is the only xai layer that knows the wire protocol of a specific
 * LLM vendor (OpenAI, Anthropic, local llama.cpp, ...).
 *
 * This header declares:
 *
 *   1. The opaque xAgentProvider handle shared by every concrete
 *      implementation.
 *   2. The vtable and callback types that concrete providers plug
 *      into. These are an **internal contract** between xai/session
 *      (the consumer) and provider implementations (the producers).
 *      End users never call them directly; they use xAgentSessionInput
 *      and friends.
 *   3. xAgentProviderDestroy, the only public lifecycle entry point
 *      that works for every provider.
 *
 * Concrete providers are created through vendor-specific headers
 * (e.g. <x/agent/provider_openai.h> exposes xAgentProviderOpenAICreate).
 */

#ifndef XAGENT_PROVIDER_H
#define XAGENT_PROVIDER_H

#include <stddef.h>
#include <x/agent/message.h>
#include <x/agent/tool.h>
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

/**
 * @brief Opaque handle to a provider instance.
 */
XDEF_HANDLE(xAgentProvider);

/**
 * @brief Reason the provider finished a single completion request.
 *
 * This is the provider-level "why did this one HTTP stream end"
 * signal. The session layer translates it into the coarser
 * xAgentDoneReason surfaced to the caller.
 */
XDEF_ENUM(xAgentProviderStopReason){
  xAgentProviderStop_EndTurn    = 0, /**< Model naturally ended the turn    */
  xAgentProviderStop_ToolUse    = 1, /**< Model requested tool execution    */
  xAgentProviderStop_MaxTokens  = 2, /**< Hit the provider's token limit    */
  xAgentProviderStop_StopSeq    = 3, /**< Matched a configured stop sequence*/
  xAgentProviderStop_PromptLong = 4, /**< Input exceeded provider context   */
  xAgentProviderStop_Error      = 5, /**< Provider/transport reported error */
  xAgentProviderStop_Cancelled  = 6, /**< Cancelled via vtable->cancel()    */
};

/**
 * @brief Parameters for a single submit() call.
 *
 * Zero-initialise for provider defaults (no sampling overrides,
 * provider's own max_tokens, no stop sequences).
 */
XDEF_STRUCT(xAgentProviderSubmitConf) {
  const char        *model;        /**< Model id (may be NULL = provider
                                        default)                          */
  const xAgentMessage  *messages;     /**< Ordered conversation history      */
  size_t             n_messages;
  const xAgentTool    **tools;        /**< Advertised tools (may be NULL)    */
  size_t             tools_count;
  double             temperature;  /**< <0 means "not set"                */
  int                max_tokens;   /**< 0 means "not set"                 */
  const char       **stop;         /**< NULL-terminated stop sequences    */
};

/* ── Provider ↔ Session streaming callbacks (internal contract) ─────── */

/**
 * @brief Delivered for each streamed text delta from the model.
 *
 * @param chunk  Byte buffer (NOT NUL-terminated).
 * @param len    Length of @p chunk in bytes.
 * @param arg    Opaque pointer supplied in xAgentProviderStreamCallbacks.
 */
typedef void (*xAgentProviderTextDeltaFunc)(const char *chunk, size_t len,
                                         void *arg);

/**
 * @brief Delivered when the provider surfaces an in-progress tool call.
 *
 * The provider is responsible for batching / de-duplicating partial
 * tool-call fragments on the wire; each invocation here represents a
 * complete tool invocation request ready to be dispatched.
 *
 * @param call  ToolUse content block (valid only during the call).
 * @param arg   Opaque pointer supplied in xAgentProviderStreamCallbacks.
 */
typedef void (*xAgentProviderToolCallFunc)(const xAgentContent *call, void *arg);

/**
 * @brief Delivered for each streamed reasoning / thinking delta.
 *
 * Thinking-capable models (kimi-k2.6, DeepSeek-R1, o1, Anthropic's
 * thinking blocks) interleave chain-of-thought with the final answer.
 * OpenAI-compatible servers carry it on `choices[].delta.reasoning_content`;
 * Anthropic carries it in a dedicated thinking content block.
 *
 * The session layer must preserve these chunks and echo them back
 * inside the assistant turn on follow-up rounds, or the server will
 * reject the follow-up (moonshot: "thinking is enabled but
 * reasoning_content is missing in assistant tool call message").
 *
 * @param chunk  Byte buffer (NOT NUL-terminated).
 * @param len    Length of @p chunk in bytes.
 * @param arg    Opaque pointer supplied in xAgentProviderStreamCallbacks.
 */
typedef void (*xAgentProviderThinkingDeltaFunc)(const char *chunk, size_t len,
                                             void *arg);

/**
 * @brief Delivered once when the submit() call terminates.
 *
 * @param reason  Why the provider stopped this round.
 * @param err     xErrno_Ok unless @p reason == xAgentProviderStop_Error.
 * @param usage   Per-request token accounting (may be NULL if the
 *                provider didn't report any; individual fields use
 *                -1 as a "not available" sentinel). Pointer is valid
 *                only for the duration of the callback — session
 *                layer must copy what it wants to keep.
 * @param errmsg  Human-readable error detail (may be NULL when err is
 *                Ok or when the provider has no additional context).
 *                Valid only for the duration of the callback.
 * @param arg     Opaque pointer supplied in xAgentProviderStreamCallbacks.
 */
typedef void (*xAgentProviderDoneFunc)(xAgentProviderStopReason reason, xErrno err,
                                    const xAgentUsage *usage, const char *errmsg,
                                    void *arg);

/**
 * @brief Streaming callbacks passed to vtable->submit().
 *
 * All callbacks are invoked on the provider's event-loop thread. Any
 * of the callbacks may be NULL (e.g. a provider that never issues
 * tool calls can leave @ref on_tool_call unused).
 */
XDEF_STRUCT(xAgentProviderStreamCallbacks) {
  xAgentProviderTextDeltaFunc     on_text;
  xAgentProviderToolCallFunc      on_tool_call;
  xAgentProviderThinkingDeltaFunc on_thinking;
  xAgentProviderDoneFunc          on_done;
};

/* ── Provider vtable (internal contract with implementations) ────────── */

/**
 * @brief Function table implemented by every concrete provider.
 *
 * Consumers (the session layer) dispatch through this vtable; they
 * never inspect the @p impl pointer directly. Implementations are
 * free to store whatever state they need behind @p impl.
 *
 * Thread model:
 * - submit / cancel / destroy are always called on the event-loop
 *   thread that owns the provider.
 * - submit returns immediately; completion is delivered through
 *   the xAgentProviderStreamCallbacks.
 * - Only one submit() may be in flight at a time per provider
 *   instance. The session layer enforces this.
 */
XDEF_STRUCT(xAgentProviderVtable) {
  /**
   * @brief Start a streaming completion.
   *
   * @param impl     Implementation state.
   * @param conf     Request configuration (must not be NULL, borrowed
   *                 only for the duration of the call).
   * @param cbs      Streaming callbacks (must not be NULL).
   * @param cb_arg   Opaque pointer forwarded to every callback.
   * @return         xErrno_Ok if the request was accepted.
   */
  xErrno (*submit)(void *impl, const xAgentProviderSubmitConf *conf,
                   const xAgentProviderStreamCallbacks *cbs, void *cb_arg);

  /**
   * @brief Cancel the in-flight submit(), if any.
   *
   * The @ref on_done callback is still delivered, with reason
   * xAgentProviderStop_Cancelled.
   *
   * @param impl  Implementation state.
   */
  void (*cancel)(void *impl);

  /**
   * @brief Tear down the implementation state.
   *
   * Called once, from xAgentProviderDestroy(). Must synchronously
   * release any resources owned by @p impl.
   *
   * @param impl  Implementation state.
   */
  void (*destroy)(void *impl);
};

/* ── Public lifecycle entry point ────────────────────────────────────── */

/**
 * @brief Destroy a provider instance.
 *
 * Cancels any in-flight submit() and releases implementation state.
 * The caller must ensure no agent or session is still holding a
 * reference to @p pvd.
 *
 * @param pvd  Provider handle (NULL is a no-op).
 */
XCAPI(void) xAgentProviderDestroy(xAgentProvider pvd);

#endif /* XAGENT_PROVIDER_H */
