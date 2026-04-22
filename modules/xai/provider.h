/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider.h - LLM provider abstraction for the xai agent core
 *
 * A provider turns a list of xAiMessage + xAiTool into a stream of
 * delta events (text chunks, emerging tool calls, completion reasons).
 * It is the only xai layer that knows the wire protocol of a specific
 * LLM vendor (OpenAI, Anthropic, local llama.cpp, ...).
 *
 * This header declares:
 *
 *   1. The opaque xAiProvider handle shared by every concrete
 *      implementation.
 *   2. The vtable and callback types that concrete providers plug
 *      into. These are an **internal contract** between xai/session
 *      (the consumer) and provider implementations (the producers).
 *      End users never call them directly; they use xAiSessionInput
 *      and friends.
 *   3. xAiProviderDestroy, the only public lifecycle entry point
 *      that works for every provider.
 *
 * Concrete providers are created through vendor-specific headers
 * (e.g. <xai/provider_openai.h> exposes xAiProviderOpenAICreate).
 */

#ifndef XAI_PROVIDER_H
#define XAI_PROVIDER_H

#include <stddef.h>
#include <xai/message.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>

/**
 * @brief Opaque handle to a provider instance.
 */
XDEF_HANDLE(xAiProvider);

/**
 * @brief Reason the provider finished a single completion request.
 *
 * This is the provider-level "why did this one HTTP stream end"
 * signal. The session layer translates it into the coarser
 * xAiDoneReason surfaced to the caller.
 */
XDEF_ENUM(xAiProviderStopReason){
  xAiProviderStop_EndTurn    = 0, /**< Model naturally ended the turn    */
  xAiProviderStop_ToolUse    = 1, /**< Model requested tool execution    */
  xAiProviderStop_MaxTokens  = 2, /**< Hit the provider's token limit    */
  xAiProviderStop_StopSeq    = 3, /**< Matched a configured stop sequence*/
  xAiProviderStop_PromptLong = 4, /**< Input exceeded provider context   */
  xAiProviderStop_Error      = 5, /**< Provider/transport reported error */
  xAiProviderStop_Cancelled  = 6, /**< Cancelled via vtable->cancel()    */
};

/**
 * @brief Parameters for a single submit() call.
 *
 * Zero-initialise for provider defaults (no sampling overrides,
 * provider's own max_tokens, no stop sequences).
 */
XDEF_STRUCT(xAiProviderSubmitConf) {
  const char        *model;        /**< Model id (may be NULL = provider
                                        default)                          */
  const xAiMessage  *messages;     /**< Ordered conversation history      */
  size_t             n_messages;
  const xAiTool    **tools;        /**< Advertised tools (may be NULL)    */
  size_t             n_tools;
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
 * @param arg    Opaque pointer supplied in xAiProviderStreamCallbacks.
 */
typedef void (*xAiProviderTextDeltaFunc)(const char *chunk, size_t len,
                                         void *arg);

/**
 * @brief Delivered when the provider surfaces an in-progress tool call.
 *
 * The provider is responsible for batching / de-duplicating partial
 * tool-call fragments on the wire; each invocation here represents a
 * complete tool invocation request ready to be dispatched.
 *
 * @param call  ToolUse content block (valid only during the call).
 * @param arg   Opaque pointer supplied in xAiProviderStreamCallbacks.
 */
typedef void (*xAiProviderToolCallFunc)(const xAiContent *call, void *arg);

/**
 * @brief Delivered once when the submit() call terminates.
 *
 * @param reason  Why the provider stopped this round.
 * @param err     xErrno_Ok unless @p reason == xAiProviderStop_Error.
 * @param arg     Opaque pointer supplied in xAiProviderStreamCallbacks.
 */
typedef void (*xAiProviderDoneFunc)(xAiProviderStopReason reason, xErrno err,
                                    void *arg);

/**
 * @brief Streaming callbacks passed to vtable->submit().
 *
 * All callbacks are invoked on the provider's event-loop thread. Any
 * of the callbacks may be NULL (e.g. a provider that never issues
 * tool calls can leave @ref on_tool_call unused).
 */
XDEF_STRUCT(xAiProviderStreamCallbacks) {
  xAiProviderTextDeltaFunc on_text;
  xAiProviderToolCallFunc  on_tool_call;
  xAiProviderDoneFunc      on_done;
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
 *   the xAiProviderStreamCallbacks.
 * - Only one submit() may be in flight at a time per provider
 *   instance. The session layer enforces this.
 */
XDEF_STRUCT(xAiProviderVtable) {
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
  xErrno (*submit)(void *impl, const xAiProviderSubmitConf *conf,
                   const xAiProviderStreamCallbacks *cbs, void *cb_arg);

  /**
   * @brief Cancel the in-flight submit(), if any.
   *
   * The @ref on_done callback is still delivered, with reason
   * xAiProviderStop_Cancelled.
   *
   * @param impl  Implementation state.
   */
  void (*cancel)(void *impl);

  /**
   * @brief Tear down the implementation state.
   *
   * Called once, from xAiProviderDestroy(). Must synchronously
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
XCAPI(void) xAiProviderDestroy(xAiProvider pvd);

#endif /* XAI_PROVIDER_H */
