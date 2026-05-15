/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider.c - Generic dispatch for the xAgentProvider abstraction
 *
 * Every concrete provider implementation (OpenAI, Anthropic, local
 * llama.cpp, ...) allocates an xAgentProvider_ struct, fills in its
 * vtable + ctx pointer, and returns it as an opaque xAgentProvider
 * handle. This TU owns just the public lifecycle entry point
 * (xAgentProviderDestroy) and the two internal dispatch helpers the
 * session layer consumes (see provider_private.h).
 */

#include "provider_private.h"

#include <stdlib.h>

xErrno ai_provider_submit(xAgentProvider pvd, const xAgentProviderSubmitConf *conf,
                          const xAgentProviderStreamCallbacks *cbs,
                          void *cb_arg) {
  struct xAgentProvider_ *base = (struct xAgentProvider_ *)pvd;
  if (!base || !base->vt || !base->vt->submit || !conf || !cbs) {
    return xErrno_InvalidArg;
  }
  return base->vt->submit(base->ctx, conf, cbs, cb_arg);
}

void ai_provider_cancel(xAgentProvider pvd) {
  struct xAgentProvider_ *base = (struct xAgentProvider_ *)pvd;
  if (!base || !base->vt || !base->vt->cancel) return;
  base->vt->cancel(base->ctx);
}

void xAgentProviderDestroy(xAgentProvider pvd) {
  struct xAgentProvider_ *base = (struct xAgentProvider_ *)pvd;
  if (!base) return;
  if (base->vt && base->vt->destroy) {
    base->vt->destroy(base->ctx);
  }
  free(base);
}
