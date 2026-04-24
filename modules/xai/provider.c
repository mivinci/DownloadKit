/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider.c - Generic dispatch for the xAiProvider abstraction
 *
 * Every concrete provider implementation (OpenAI, Anthropic, local
 * llama.cpp, ...) allocates an xAiProvider_ struct, fills in its
 * vtable + ctx pointer, and returns it as an opaque xAiProvider
 * handle. This TU owns just the public lifecycle entry point
 * (xAiProviderDestroy) and the two internal dispatch helpers the
 * session layer consumes (see provider_private.h).
 */

#include "provider_private.h"

#include <stdlib.h>

xErrno ai_provider_submit(xAiProvider pvd, const xAiProviderSubmitConf *conf,
                          const xAiProviderStreamCallbacks *cbs,
                          void *cb_arg) {
  struct xAiProvider_ *base = (struct xAiProvider_ *)pvd;
  if (!base || !base->vt || !base->vt->submit || !conf || !cbs) {
    return xErrno_InvalidArg;
  }
  return base->vt->submit(base->ctx, conf, cbs, cb_arg);
}

void ai_provider_cancel(xAiProvider pvd) {
  struct xAiProvider_ *base = (struct xAiProvider_ *)pvd;
  if (!base || !base->vt || !base->vt->cancel) return;
  base->vt->cancel(base->ctx);
}

void xAiProviderDestroy(xAiProvider pvd) {
  struct xAiProvider_ *base = (struct xAiProvider_ *)pvd;
  if (!base) return;
  if (base->vt && base->vt->destroy) {
    base->vt->destroy(base->ctx);
  }
  free(base);
}
