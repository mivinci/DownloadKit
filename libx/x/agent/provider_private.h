/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider_private.h - Shared internals for the xai provider layer
 *
 * Declares the concrete layout of xAgentProvider (vtable pointer + ctx
 * pointer) and a handful of tiny dispatch helpers that callers in the
 * xai module (notably session.c) use instead of touching the vtable
 * directly. Not installed; not part of the public API.
 */

#ifndef XAGENT_PROVIDER_PRIVATE_H
#define XAGENT_PROVIDER_PRIVATE_H

#include <x/agent/provider.h>
#include <x/base/base.h>
#include <x/base/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Concrete provider layout.
 *
 * Every concrete provider (OpenAI, Anthropic, ...) allocates exactly
 * this struct and fills in @p vt + @p ctx. The public xAgentProvider
 * handle is just a pointer to this struct cast to void*.
 */
struct xAgentProvider_ {
  const xAgentProviderVtable *vt;  /**< Static vtable (never NULL).         */
  void                    *ctx; /**< Provider-owned private context.     */
};

/**
 * @brief Dispatch to ctx->vt->submit(). Asserts on NULL inputs.
 *
 * The session layer uses this instead of hand-dispatching the vtable,
 * both for readability and to keep the single-in-flight invariant in
 * one place should we decide to enforce it here later.
 */
xErrno ai_provider_submit(xAgentProvider pvd, const xAgentProviderSubmitConf *conf,
                          const xAgentProviderStreamCallbacks *cbs, void *cb_arg);

/**
 * @brief Dispatch to ctx->vt->cancel(). No-op if @p pvd is NULL.
 */
void ai_provider_cancel(xAgentProvider pvd);

#ifdef __cplusplus
}
#endif

#endif /* XAGENT_PROVIDER_PRIVATE_H */
