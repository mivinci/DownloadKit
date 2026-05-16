/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_eval.c - xjs script evaluation and GC entry points.
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>

#include "quickjs.h"

bool xJSCheckScriptSyntax(xJSContextRef c, xJSStringRef script,
                          xJSStringRef sourceURL, int startingLineNumber,
                          xJSValueRef *exc) {
  (void)startingLineNumber;
  if (!script) return false;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(script->data, script->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return false;
  xjs_utf16_to_utf8(script->data, script->length, buf, bytes);
  buf[bytes] = 0;
  char *url  = NULL;
  if (sourceURL) {
    size_t urlBytes =
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, NULL, 0);
    url = (char *)malloc(urlBytes + 1);
    if (url) {
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, url, urlBytes);
      url[urlBytes] = 0;
    }
  }
  JSValue v = JS_Eval(q, buf, bytes, url ? url : "<xjs>",
                      JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  free(buf);
  free(url);
  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exc);
    return false;
  }
  JS_FreeValue(q, v);
  return true;
}

xJSValueRef xJSEvaluateScript(xJSContextRef c, xJSStringRef script,
                              xJSObjectRef thisObject, xJSStringRef sourceURL,
                              int startingLineNumber, xJSValueRef *exc) {
  (void)startingLineNumber;
  if (!script) return NULL;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(script->data, script->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return NULL;
  xjs_utf16_to_utf8(script->data, script->length, buf, bytes);
  buf[bytes] = 0;
  char *url  = NULL;
  if (sourceURL) {
    size_t urlBytes =
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, NULL, 0);
    url = (char *)malloc(urlBytes + 1);
    if (url) {
      xjs_utf16_to_utf8(sourceURL->data, sourceURL->length, url, urlBytes);
      url[urlBytes] = 0;
    }
  }
  /* JSC lets callers pin `this` for top-level evaluation.  QuickJS's
   * JS_EvalThis is the direct analogue; passing JS_UNDEFINED recovers
   * plain JS_Eval semantics (this == globalThis).  We borrow the qv
   * from the slot — JS_EvalThis takes JSValueConst and does not
   * consume the reference. */
  JSValueConst this_obj =
    thisObject ? xjs_slot_qv((xJSValueRef)thisObject) : JS_UNDEFINED;
  JSValue v = JS_EvalThis(q, this_obj, buf, bytes, url ? url : "<xjs>",
                          JS_EVAL_TYPE_GLOBAL);
  free(buf);
  free(url);
  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return xjs_slot_make(q, v);
}

void xJSGarbageCollect(xJSContextRef c) {
  if (c && c->group && c->group->rt) JS_RunGC(c->group->rt);
}

int xJSContextDrainPendingJobs(xJSContextRef c, xJSValueRef *exception) {
  if (!c || !c->group || !c->group->rt) return 0;
  JSRuntime *rt       = c->group->rt;
  int        executed = 0;
  for (;;) {
    JSContext *jctx = NULL;
    int        r    = JS_ExecutePendingJob(rt, &jctx);
    if (r == 0) break;           /* no more jobs */
    if (r < 0) {
      /* A job returned JSException.  Defensive: QuickJS's standard
       * promise_reaction_job internally catches throws and turns
       * them into rejections, so this branch is hard to hit from
       * pure JS — it mostly guards against future job kinds and
       * host-registered jobs (JS_EnqueueJob) whose callbacks let
       * exceptions escape. */
      if (jctx) xjs_propagate_exception(jctx, exception);
      break;
    }
    ++executed;
  }
  return executed;
}

bool xJSContextHasPendingJobs(xJSContextRef c) {
  if (!c || !c->group || !c->group->rt) return false;
  return JS_IsJobPending(c->group->rt);
}
