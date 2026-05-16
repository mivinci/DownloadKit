/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_module.c - ES module support: evaluation, loader trampoline,
 *               and the synchronous Promise-await helper.
 *
 * The module surface is documented in js.h.  Design notes:
 *
 *   - Specifier normalisation (resolving "./x" relative to the
 *     importer) is delegated to QuickJS's default implementation
 *     via JS_SetModuleLoaderFunc2(rt, NULL, …).  The user-facing
 *     loader callback therefore only ever sees normalised names,
 *     matching JSC's JSModuleLoaderDelegate contract.
 *
 *   - We route every runtime's `import` through a single trampoline
 *     that recovers the owning xjs context from QuickJS's per-
 *     context opaque.  The callback + opaque pair lives on the
 *     xjs context; installing NULL unsubscribes.
 *
 *   - xJSAwaitPromise drains the job queue until settlement.  It
 *     is intentionally modelled as a general utility (not tied to
 *     modules) so embedders can reuse it for any host-side Promise.
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* ═══════════════════════════════════════════════════════════════════
 * Module loader trampoline
 * ═══════════════════════════════════════════════════════════════════ */

JSModuleDef *xjs_module_loader_trampoline(JSContext *qctx,
                                          const char *module_name,
                                          void       *runtime_opaque,
                                          JSValueConst attributes) {
  (void)runtime_opaque;
  (void)attributes;

  struct OpaqueXJSContext *c = xjs_ctx_from_q(qctx);
  if (!c || !c->module_load_cb) {
    JS_ThrowReferenceError(
      qctx, "could not load module '%s': no loader installed", module_name);
    return NULL;
  }

  xJSStringRef src = c->module_load_cb(c, module_name, c->module_load_opaque);
  if (!src) {
    JS_ThrowReferenceError(qctx, "could not load module '%s'", module_name);
    return NULL;
  }

  /* Transcode UTF-16 → UTF-8 for QuickJS's byte-oriented compiler.
   * We go through malloc rather than the QuickJS allocator because
   * JS_Eval copies the buffer internally; freeing with libc free()
   * keeps lifetime reasoning local. */
  size_t nbytes = xjs_utf16_to_utf8(src->data, src->length, NULL, 0);
  char  *buf    = (char *)malloc(nbytes + 1);
  if (!buf) {
    xJSStringRelease(src);
    JS_ThrowOutOfMemory(qctx);
    return NULL;
  }
  xjs_utf16_to_utf8(src->data, src->length, buf, nbytes);
  buf[nbytes] = 0;

  /* Compile-only: produces a JS_TAG_MODULE value whose payload
   * pointer is the JSModuleDef* QuickJS expects us to return.
   * This is the exact pattern upstream's js_module_load uses. */
  JSValue mv = JS_Eval(qctx, buf, nbytes, module_name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

  free(buf);
  xJSStringRelease(src);

  if (JS_IsException(mv)) return NULL;
  JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(mv);
  JS_FreeValue(qctx, mv);
  return m;
}

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

bool xJSDetectModule(const char *source, size_t length) {
  if (!source) return false;
  return JS_DetectModule(source, length);
}

xJSValueRef xJSEvaluateModule(xJSContextRef c, xJSStringRef script,
                              xJSStringRef sourceURL, xJSValueRef *exc) {
  if (!script) return NULL;
  JSContext *q = xjs_ctx_of(c);
  if (!q) return NULL;

  size_t nbytes = xjs_utf16_to_utf8(script->data, script->length, NULL, 0);
  char  *buf    = (char *)malloc(nbytes + 1);
  if (!buf) return NULL;
  xjs_utf16_to_utf8(script->data, script->length, buf, nbytes);
  buf[nbytes] = 0;

  char *url = NULL;
  if (sourceURL) {
    size_t urlbytes =
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, NULL, 0);
    url = (char *)malloc(urlbytes + 1);
    if (url) {
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, url, urlbytes);
      url[urlbytes] = 0;
    }
  }

  /* JS_Eval with JS_EVAL_TYPE_MODULE returns the module-evaluation
   * Promise directly (QuickJS boxes it for us at line 36545 of
   * quickjs.c when JS_EVAL_FLAG_ASYNC is implied by module type).
   * Imports are resolved through our trampoline above. */
  JSValue v =
    JS_Eval(q, buf, nbytes, url ? url : "<xjs>", JS_EVAL_TYPE_MODULE);

  free(buf);
  free(url);

  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return xjs_slot_make(q, v);
}

xJSValueRef xJSAwaitPromise(xJSContextRef c, xJSValueRef promise,
                            xJSValueRef *exc) {
  if (!c || !promise) return NULL;
  JSContext *q  = xjs_ctx_of(c);
  JSValue    pv = xjs_slot_qv(promise);

  /* Not a Promise?  Return the value unchanged (retained by caller,
   * so we just bump the slot refcount). */
  if (JS_PromiseState(q, pv) == JS_PROMISE_NOT_A_PROMISE) {
    xjs_slot_retain(promise);
    return promise;
  }

  /* Drain until the promise leaves the "pending" state.  We
   * inspect JS_PromiseState after every executed job rather than
   * trying to hook into promise resolution directly — simpler, and
   * avoids QuickJS internals. */
  JSRuntime *rt = c->group->rt;
  while (JS_PromiseState(q, pv) == JS_PROMISE_PENDING) {
    JSContext *jctx = NULL;
    int        r    = JS_ExecutePendingJob(rt, &jctx);
    if (r == 0) {
      /* Job queue drained but promise still pending — this means
       * an external resolver that will never fire (e.g. a host
       * who forgot to call resolve()).  Report as exception so the
       * caller doesn't silently spin. */
      JS_ThrowInternalError(q, "await: promise never settled");
      xjs_propagate_exception(q, exc);
      return NULL;
    }
    if (r < 0) {
      if (jctx) xjs_propagate_exception(jctx, exc);
      return NULL;
    }
  }

  JSValue result = JS_PromiseResult(q, pv); /* fresh reference */
  if (JS_PromiseState(q, pv) == JS_PROMISE_REJECTED) {
    if (exc) *exc = xjs_slot_make(q, result);
    else     JS_FreeValue(q, result);
    return NULL;
  }
  return xjs_slot_make(q, result);
}
