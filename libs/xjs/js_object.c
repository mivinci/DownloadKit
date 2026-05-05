/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_object.c - xjs object operations and property-name
 * array/accumulator.  Most Make* specialisations are stubs until we
 * start binding native modules; kept here so the object surface lives
 * in one place.
 */

#include "js.h"
#include "js_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* ═══════════════════════════════════════════════════════════════════
 * kXJSClassDefinitionEmpty
 * ═══════════════════════════════════════════════════════════════════ */

const xJSClassDefinition kXJSClassDefinitionEmpty = {0};

/* ═══════════════════════════════════════════════════════════════════
 * Host-function machinery
 *
 * xJSObjectMakeFunctionWithCallback needs to turn a JSC-style
 * xJSObjectCallAsFunctionCallback into something JavaScript can
 * invoke.  We model it as a QuickJS class whose `.call` slot is our
 * trampoline; each instance carries the user function pointer (plus
 * its name) in its JS_SetOpaque slot.  QuickJS's JS_IsFunction
 * returns true for any object whose class has a non-NULL `.call`,
 * so callers get a value that behaves indistinguishably from a
 * normal function.
 *
 * The JSClassID is allocated lazily on first use and then
 * registered against every JSRuntime we encounter (idempotent, same
 * pattern as xjs_class_ensure_registered()).
 * ═══════════════════════════════════════════════════════════════════ */

static JSClassID s_host_fn_class_id = 0;

struct xjs_host_fn {
  xJSObjectCallAsFunctionCallback fn;
  xJSStringRef                    name; /* retained; may be NULL */
};

static JSValue xjs_host_fn_call(JSContext *ctx, JSValueConst func_obj,
                                JSValueConst this_val, int argc,
                                JSValueConst *argv, int flags) {
  (void)flags;
  struct xjs_host_fn *h =
    (struct xjs_host_fn *)JS_GetOpaque(func_obj, s_host_fn_class_id);
  if (!h || !h->fn) return JS_UNDEFINED;

  struct OpaqueXJSContext *xctx = xjs_ctx_from_q(ctx);

  /* Box func_obj and this_val into xjs slots (each taking one ref). */
  xJSObjectRef xfunc =
    (xJSObjectRef)xjs_slot_make(ctx, JS_DupValue(ctx, func_obj));
  xJSObjectRef xthis =
    (xJSObjectRef)xjs_slot_make(ctx, JS_DupValue(ctx, this_val));

  /* Box arguments.  For zero-arg calls we pass NULL to match JSC. */
  xJSValueRef *xargs = NULL;
  if (argc > 0) {
    xargs = (xJSValueRef *)malloc((size_t)argc * sizeof(xJSValueRef));
    if (!xargs) {
      xjs_slot_release((xJSValueRef)xfunc);
      xjs_slot_release((xJSValueRef)xthis);
      return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < argc; ++i)
      xargs[i] = xjs_slot_make(ctx, JS_DupValue(ctx, argv[i]));
  }

  xJSValueRef exc = NULL;
  xJSValueRef ret = h->fn((xJSContextRef)xctx, xfunc, xthis, (size_t)argc,
                          (const xJSValueRef *)xargs, &exc);

  /* Convert result back into a QuickJS JSValue.
   *
   * Ownership contract: the returned slot's ref is handed to the
   * VM — we release it after extracting the underlying JSValue.
   * User callbacks that wish to hand back one of the slots we
   * passed in (xfunc / xthis / one of xargs) must not be charged
   * for it; we detect that aliasing below so the automatic
   * xjs_slot_release() at the bottom of the function still
   * balances without dropping the slot's sole reference early. */
  int ret_aliases_input = 0;
  if (ret) {
    if (ret == (xJSValueRef)xfunc || ret == (xJSValueRef)xthis) {
      ret_aliases_input = 1;
    } else {
      for (int i = 0; i < argc; ++i)
        if (ret == xargs[i]) { ret_aliases_input = 1; break; }
    }
  }

  JSValue qret;
  if (exc) {
    JSValue qexc = JS_DupValue(ctx, xjs_slot_qv(exc));
    xjs_slot_release(exc);
    qret = JS_Throw(ctx, qexc);
  } else if (ret) {
    qret = JS_DupValue(ctx, xjs_slot_qv(ret));
    if (!ret_aliases_input) xjs_slot_release(ret);
  } else {
    qret = JS_UNDEFINED;
  }

  if (xargs) {
    for (int i = 0; i < argc; ++i) xjs_slot_release(xargs[i]);
    free(xargs);
  }
  xjs_slot_release((xJSValueRef)xfunc);
  xjs_slot_release((xJSValueRef)xthis);
  return qret;
}

static void xjs_host_fn_finalize(JSRuntime *rt, JSValue val) {
  (void)rt;
  struct xjs_host_fn *h =
    (struct xjs_host_fn *)JS_GetOpaque(val, s_host_fn_class_id);
  if (!h) return;
  if (h->name) xJSStringRelease(h->name);
  free(h);
}

static int xjs_host_fn_ensure_registered(JSRuntime *rt) {
  if (s_host_fn_class_id == 0) JS_NewClassID(rt, &s_host_fn_class_id);
  if (JS_IsRegisteredClass(rt, s_host_fn_class_id)) return 0;
  JSClassDef def;
  memset(&def, 0, sizeof(def));
  def.class_name = "xJSHostFunction";
  def.finalizer  = xjs_host_fn_finalize;
  def.call       = xjs_host_fn_call;
  return JS_NewClass(rt, s_host_fn_class_id, &def) == 0 ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Host-constructor machinery
 *
 * Mirror of the host-function trampoline, but wired up for `new X()`
 * invocations.  We keep a separate QuickJS class so the two roles
 * stay cleanly separated (different callback signatures, different
 * return-type handling: constructors yield JSObjectRef, functions
 * yield JSValueRef).
 *
 * QuickJS delivers `new X()` through the same `.call` slot with
 * `flags & JS_CALL_FLAG_CONSTRUCTOR` set — but we set the
 * constructor bit on the object itself (JS_SetConstructorBit) so
 * JS_IsConstructor returns true and the language accepts `new`.
 * ═══════════════════════════════════════════════════════════════════ */

static JSClassID s_host_ctor_class_id = 0;

struct xjs_host_ctor {
  xJSObjectCallAsConstructorCallback fn;
  xJSClassRef                        jsClass; /* retained; may be NULL */
};

static JSValue xjs_host_ctor_call(JSContext *ctx, JSValueConst func_obj,
                                  JSValueConst this_val, int argc,
                                  JSValueConst *argv, int flags) {
  (void)this_val;
  (void)flags; /* We treat plain-call and `new` identically here —
                * JSC's JSObjectCallAsConstructorCallback does not
                * distinguish, it's always invoked as a constructor. */

  struct xjs_host_ctor *h =
    (struct xjs_host_ctor *)JS_GetOpaque(func_obj, s_host_ctor_class_id);
  if (!h || !h->fn)
    return JS_ThrowTypeError(ctx, "constructor has no callback");

  struct OpaqueXJSContext *xctx = xjs_ctx_from_q(ctx);

  xJSObjectRef xctor =
    (xJSObjectRef)xjs_slot_make(ctx, JS_DupValue(ctx, func_obj));

  xJSValueRef *xargs = NULL;
  if (argc > 0) {
    xargs = (xJSValueRef *)malloc((size_t)argc * sizeof(xJSValueRef));
    if (!xargs) {
      xjs_slot_release((xJSValueRef)xctor);
      return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < argc; ++i)
      xargs[i] = xjs_slot_make(ctx, JS_DupValue(ctx, argv[i]));
  }

  xJSValueRef  exc = NULL;
  xJSObjectRef ret = h->fn((xJSContextRef)xctx, xctor, (size_t)argc,
                           (const xJSValueRef *)xargs, &exc);

  int ret_aliases_input = 0;
  if (ret) {
    if (ret == xctor) {
      ret_aliases_input = 1;
    } else {
      for (int i = 0; i < argc; ++i)
        if ((xJSValueRef)ret == xargs[i]) {
          ret_aliases_input = 1;
          break;
        }
    }
  }

  JSValue qret;
  if (exc) {
    JSValue qexc = JS_DupValue(ctx, xjs_slot_qv(exc));
    xjs_slot_release(exc);
    qret = JS_Throw(ctx, qexc);
  } else if (ret) {
    qret = JS_DupValue(ctx, xjs_slot_qv((xJSValueRef)ret));
    if (!ret_aliases_input) xjs_slot_release((xJSValueRef)ret);
  } else {
    /* Callback returned no object and no exception — JSC treats this
     * as a TypeError in strict-construct contexts; we match. */
    qret = JS_ThrowTypeError(ctx, "constructor returned no object");
  }

  if (xargs) {
    for (int i = 0; i < argc; ++i) xjs_slot_release(xargs[i]);
    free(xargs);
  }
  xjs_slot_release((xJSValueRef)xctor);
  return qret;
}

static void xjs_host_ctor_finalize(JSRuntime *rt, JSValue val) {
  (void)rt;
  struct xjs_host_ctor *h =
    (struct xjs_host_ctor *)JS_GetOpaque(val, s_host_ctor_class_id);
  if (!h) return;
  if (h->jsClass) xJSClassRelease(h->jsClass);
  free(h);
}

static int xjs_host_ctor_ensure_registered(JSRuntime *rt) {
  if (s_host_ctor_class_id == 0) JS_NewClassID(rt, &s_host_ctor_class_id);
  if (JS_IsRegisteredClass(rt, s_host_ctor_class_id)) return 0;
  JSClassDef def;
  memset(&def, 0, sizeof(def));
  def.class_name = "xJSHostConstructor";
  def.finalizer  = xjs_host_ctor_finalize;
  def.call       = xjs_host_ctor_call;
  return JS_NewClass(rt, s_host_ctor_class_id, &def) == 0 ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Object — Make*
 * ═══════════════════════════════════════════════════════════════════ */

xJSObjectRef xJSObjectMake(xJSContextRef c, xJSClassRef k, void *data) {
  JSContext *q = xjs_ctx_of(c);
  if (!q) return NULL;

  /* Plain Object — JSC ignores `data` when no class is given. */
  if (!k) {
    (void)data;
    return (xJSObjectRef)xjs_slot_make(q, JS_NewObject(q));
  }

  /* Class-backed Object: lazy-register on this runtime, create the
   * instance, attach the xjs_native_priv via JS_SetOpaque. */
  JSRuntime *rt = JS_GetRuntime(q);
  if (xjs_class_ensure_registered(k, rt) != 0) return NULL;

  JSValue obj = JS_NewObjectClass(q, (int)k->qclass);
  if (JS_IsException(obj)) return NULL;

  struct xjs_native_priv *priv =
    (struct xjs_native_priv *)malloc(sizeof(*priv));
  if (!priv) {
    JS_FreeValue(q, obj);
    return NULL;
  }
  priv->jsclass = k;
  priv->data    = data;
  JS_SetOpaque(obj, priv);

  /* Fire user-provided initialize callback if any. */
  if (k->def.initialize) {
    /* initialize gets the stable boxed object; the slot takes one
     * reference which we drop before returning so the return value
     * still owns the single reference produced by JS_NewObjectClass. */
    xJSObjectRef tmp = (xJSObjectRef)xjs_slot_make(q, JS_DupValue(q, obj));
    if (tmp) {
      k->def.initialize((xJSContextRef)c, tmp);
      xjs_slot_release((xJSValueRef)tmp);
    }
  }
  return (xJSObjectRef)xjs_slot_make(q, obj);
}

xJSObjectRef
xJSObjectMakeFunctionWithCallback(xJSContextRef c, xJSStringRef name,
                                  xJSObjectCallAsFunctionCallback fn) {
  JSContext *q = xjs_ctx_of(c);
  if (!q || !fn) return NULL;

  JSRuntime *rt = JS_GetRuntime(q);
  if (xjs_host_fn_ensure_registered(rt) != 0) return NULL;

  JSValue obj = JS_NewObjectClass(q, (int)s_host_fn_class_id);
  if (JS_IsException(obj)) return NULL;

  struct xjs_host_fn *h =
    (struct xjs_host_fn *)calloc(1, sizeof(*h));
  if (!h) {
    JS_FreeValue(q, obj);
    return NULL;
  }
  h->fn   = fn;
  h->name = name ? xJSStringRetain(name) : NULL;
  JS_SetOpaque(obj, h);

  /* Mark the object as callable-but-not-constructable (default for
   * JS_NewObjectClass with a .call slot is "callable"; no extra bit
   * needed here).  Expose the caller-supplied name via the standard
   * "name" property to match JSC's semantics. */
  if (name) {
    JSValue jname = xjs_qv_from_string(q, name);
    JS_DefinePropertyValueStr(q, obj, "name", jname,
                              JS_PROP_CONFIGURABLE);
  }
  return (xJSObjectRef)xjs_slot_make(q, obj);
}

xJSObjectRef xJSObjectMakeConstructor(xJSContextRef c, xJSClassRef k,
                                      xJSObjectCallAsConstructorCallback fn) {
  JSContext *q = xjs_ctx_of(c);
  if (!q || !fn) return NULL;

  JSRuntime *rt = JS_GetRuntime(q);
  if (xjs_host_ctor_ensure_registered(rt) != 0) return NULL;

  JSValue obj = JS_NewObjectClass(q, (int)s_host_ctor_class_id);
  if (JS_IsException(obj)) return NULL;

  struct xjs_host_ctor *h =
    (struct xjs_host_ctor *)calloc(1, sizeof(*h));
  if (!h) {
    JS_FreeValue(q, obj);
    return NULL;
  }
  h->fn      = fn;
  h->jsClass = k ? xJSClassRetain(k) : NULL;
  JS_SetOpaque(obj, h);

  /* Mark as constructor so JS_IsConstructor returns true and `new`
   * dispatches through our .call trampoline with the constructor
   * flag set. */
  JS_SetConstructorBit(q, obj, 1);
  return (xJSObjectRef)xjs_slot_make(q, obj);
}

xJSObjectRef xJSObjectMakeArray(xJSContextRef c, size_t argc,
                                const xJSValueRef argv[], xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  JSValue    a = JS_NewArray(q);
  if (JS_IsException(a)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  for (size_t i = 0; i < argc; ++i) {
    JSValue v = JS_DupValue(q, xjs_slot_qv(argv[i]));
    JS_SetPropertyUint32(q, a, (uint32_t)i, v);
  }
  return (xJSObjectRef)xjs_slot_make(q, a);
}

/* Invoke `new <global_name>(...args)` and return the resulting
 * object slot.  Used by MakeDate / MakeRegExp where QuickJS either
 * doesn't expose a direct C API (RegExp) or the JS path covers far
 * more argument shapes than the C one (Date).  Returns NULL on
 * exception or if the named global isn't a constructor. */
static xJSObjectRef xjs_construct_global(JSContext *q, const char *name,
                                         size_t argc, const xJSValueRef argv[],
                                         xJSValueRef *exc) {
  JSValue global = JS_GetGlobalObject(q);
  JSValue ctor   = JS_GetPropertyStr(q, global, name);
  JS_FreeValue(q, global);
  if (JS_IsException(ctor)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  if (JS_IsConstructor(q, ctor) != 1) {
    JS_FreeValue(q, ctor);
    if (exc) {
      JSValue e = JS_ThrowTypeError(q, "%s is not a constructor", name);
      (void)e; /* JS_Throw installed the current exception */
      xjs_propagate_exception(q, exc);
    }
    return NULL;
  }

  JSValue *qargs = NULL;
  if (argc > 0) {
    qargs = (JSValue *)malloc(argc * sizeof(JSValue));
    if (!qargs) {
      JS_FreeValue(q, ctor);
      return NULL;
    }
    for (size_t i = 0; i < argc; ++i) qargs[i] = xjs_slot_qv(argv[i]);
  }

  JSValue r = JS_CallConstructor(q, ctor, (int)argc, qargs);
  free(qargs);
  JS_FreeValue(q, ctor);

  if (JS_IsException(r)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return (xJSObjectRef)xjs_slot_make(q, r);
}

xJSObjectRef xJSObjectMakeDate(xJSContextRef c, size_t argc,
                               const xJSValueRef argv[], xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!q) return NULL;

  /* Fast-path: `new Date()` with no arguments becomes "now".  We
   * still defer to the JS constructor for parity with argc>=1
   * behaviour (string parsing, multi-arg y/m/d/...). */
  return xjs_construct_global(q, "Date", argc, argv, exc);
}

xJSObjectRef xJSObjectMakeError(xJSContextRef c, size_t argc,
                                const xJSValueRef argv[], xJSValueRef *exc) {
  (void)exc;
  JSContext *q = xjs_ctx_of(c);
  JSValue    e = JS_NewError(q);
  if (argc >= 1 && argv && argv[0]) {
    JSValue msg = JS_DupValue(q, xjs_slot_qv(argv[0]));
    JS_DefinePropertyValueStr(q, e, "message", msg, 0);
  }
  return (xJSObjectRef)xjs_slot_make(q, e);
}

xJSObjectRef xJSObjectMakeRegExp(xJSContextRef c, size_t argc,
                                 const xJSValueRef argv[], xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!q) return NULL;
  return xjs_construct_global(q, "RegExp", argc, argv, exc);
}

xJSObjectRef xJSObjectMakeDeferredPromise(xJSContextRef c, xJSObjectRef *res,
                                          xJSObjectRef *rej, xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!q) return NULL;

  JSValue resolving[2];
  JSValue promise = JS_NewPromiseCapability(q, resolving);
  if (JS_IsException(promise)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }

  /* resolving[0] = resolve, resolving[1] = reject.  QuickJS gives
   * us owning references; hand each over to its own slot.  If the
   * caller didn't ask for one we free it immediately so no leak. */
  if (res)
    *res = (xJSObjectRef)xjs_slot_make(q, resolving[0]);
  else
    JS_FreeValue(q, resolving[0]);

  if (rej)
    *rej = (xJSObjectRef)xjs_slot_make(q, resolving[1]);
  else
    JS_FreeValue(q, resolving[1]);

  return (xJSObjectRef)xjs_slot_make(q, promise);
}

/* Helper: convert an xJSStringRef (UTF-16) to a freshly-allocated
 * UTF-8 C string.  Caller must free.  Returns NULL on OOM or NULL
 * input. */
static char *xjs_str_to_utf8(xJSStringRef s) {
  if (!s) return NULL;
  size_t bytes = xjs_utf16_to_utf8(s->data, s->length, NULL, 0);
  char  *buf   = (char *)malloc(bytes + 1);
  if (!buf) return NULL;
  xjs_utf16_to_utf8(s->data, s->length, buf, bytes);
  buf[bytes] = 0;
  return buf;
}

xJSObjectRef xJSObjectMakeFunction(xJSContextRef c, xJSStringRef name,
                                   unsigned           parameterCount,
                                   const xJSStringRef parameterNames[],
                                   xJSStringRef body, xJSStringRef sourceURL,
                                   int          startingLineNumber,
                                   xJSValueRef *exception) {
  (void)startingLineNumber; /* [TODO] pipe through JS_Eval when API exposes it */
  JSContext *q = xjs_ctx_of(c);
  if (!q || !body) return NULL;

  /* Build source:  (function [name](p1,p2,...){ body })
   *
   * Using an expression form wrapped in parens so JS_Eval's global
   * mode returns the function value directly.  */
  char  *body_s = xjs_str_to_utf8(body);
  char  *name_s = xjs_str_to_utf8(name); /* may be NULL */
  char **params = NULL;
  if (parameterCount) {
    params = (char **)calloc(parameterCount, sizeof(char *));
    if (!params) goto oom;
    for (unsigned i = 0; i < parameterCount; ++i) {
      params[i] = xjs_str_to_utf8(parameterNames[i]);
      if (!params[i]) goto oom;
    }
  }

  /* Compute size: "(function " + name + "(" + params... + "){" + body + "})" */
  size_t total = 32; /* wrapper fixed chars + safety */
  if (name_s) total += strlen(name_s);
  for (unsigned i = 0; i < parameterCount; ++i)
    total += strlen(params[i]) + 1; /* + comma */
  if (body_s) total += strlen(body_s);

  char *src = (char *)malloc(total);
  if (!src) goto oom;
  char *p = src;
  p += sprintf(p, "(function %s(", name_s ? name_s : "");
  for (unsigned i = 0; i < parameterCount; ++i)
    p += sprintf(p, "%s%s", i ? "," : "", params[i]);
  p += sprintf(p, "){%s})", body_s ? body_s : "");

  char *url = xjs_str_to_utf8(sourceURL);
  JSValue v =
    JS_Eval(q, src, (size_t)(p - src), url ? url : "<xjs_fn>",
            JS_EVAL_TYPE_GLOBAL);
  free(src);
  free(url);
  free(body_s);
  free(name_s);
  if (params) {
    for (unsigned i = 0; i < parameterCount; ++i) free(params[i]);
    free(params);
  }

  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exception);
    return NULL;
  }
  return (xJSObjectRef)xjs_slot_make(q, v);

oom:
  free(body_s);
  free(name_s);
  if (params) {
    for (unsigned i = 0; i < parameterCount; ++i) free(params[i]);
    free(params);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * Object — prototype & properties
 * ═══════════════════════════════════════════════════════════════════ */

xJSValueRef xJSObjectGetPrototype(xJSContextRef c, xJSObjectRef o) {
  JSContext *q = xjs_ctx_of(c);
  if (!o) return NULL;
  return xjs_slot_make(q, JS_GetPrototype(q, xjs_slot_qv((xJSValueRef)o)));
}

void xJSObjectSetPrototype(xJSContextRef c, xJSObjectRef o, xJSValueRef v) {
  if (!o || !v) return;
  JS_SetPrototype(xjs_ctx_of(c), xjs_slot_qv((xJSValueRef)o), xjs_slot_qv(v));
}

bool xJSObjectHasProperty(xJSContextRef c, xJSObjectRef o, xJSStringRef name) {
  if (!o || !name) return false;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(name->data, name->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return false;
  xjs_utf16_to_utf8(name->data, name->length, buf, bytes);
  buf[bytes]  = 0;
  JSAtom atom = JS_NewAtom(q, buf);
  int    has  = JS_HasProperty(q, xjs_slot_qv((xJSValueRef)o), atom);
  JS_FreeAtom(q, atom);
  free(buf);
  return has == 1;
}

xJSValueRef xJSObjectGetProperty(xJSContextRef c, xJSObjectRef o,
                                 xJSStringRef name, xJSValueRef *exc) {
  if (!o || !name) return NULL;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(name->data, name->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return NULL;
  xjs_utf16_to_utf8(name->data, name->length, buf, bytes);
  buf[bytes] = 0;
  JSValue v  = JS_GetPropertyStr(q, xjs_slot_qv((xJSValueRef)o), buf);
  free(buf);
  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return xjs_slot_make(q, v);
}

void xJSObjectSetProperty(xJSContextRef c, xJSObjectRef o, xJSStringRef name,
                          xJSValueRef v, xJSPropertyAttributes attrs,
                          xJSValueRef *exc) {
  if (!o || !name) return;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(name->data, name->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return;
  xjs_utf16_to_utf8(name->data, name->length, buf, bytes);
  buf[bytes] = 0;
  JSValue qv = v ? JS_DupValue(q, xjs_slot_qv(v)) : JS_UNDEFINED;

  /* JSC attribute flags are "remove" bits relative to the default
   * data-descriptor (writable+enumerable+configurable).  When no flag
   * is set we keep the historical JS_SetPropertyStr path, which
   * preserves assignment semantics (prototype setters fire, existing
   * descriptor flags are respected, etc.).  When *any* flag is set
   * the JSC contract is that the caller wants to pin descriptor
   * shape, so we switch to JS_DefinePropertyValue — which creates or
   * replaces the own property with the exact flags we specify. */
  int    r      = 0;
  JSAtom consumedAtom = JS_ATOM_NULL;
  if (attrs == kXJSPropertyAttributeNone) {
    r = JS_SetPropertyStr(q, xjs_slot_qv((xJSValueRef)o), buf, qv);
    /* JS_SetPropertyStr takes ownership of qv regardless of success. */
  } else {
    int flags = JS_PROP_HAS_VALUE |
                JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE |
                JS_PROP_HAS_CONFIGURABLE;
    if (!(attrs & kXJSPropertyAttributeReadOnly))   flags |= JS_PROP_WRITABLE;
    if (!(attrs & kXJSPropertyAttributeDontEnum))   flags |= JS_PROP_ENUMERABLE;
    if (!(attrs & kXJSPropertyAttributeDontDelete)) flags |= JS_PROP_CONFIGURABLE;
    JSAtom atom = JS_NewAtom(q, buf);
    consumedAtom = atom;
    /* JS_DefinePropertyValue consumes qv on both success and failure. */
    r = JS_DefinePropertyValue(q, xjs_slot_qv((xJSValueRef)o), atom, qv,
                               flags);
    JS_FreeAtom(q, consumedAtom);
  }
  if (r < 0) {
    xjs_propagate_exception(q, exc);
  }
  free(buf);
}

bool xJSObjectDeleteProperty(xJSContextRef c, xJSObjectRef o, xJSStringRef name,
                             xJSValueRef *exc) {
  if (!o || !name) return false;
  JSContext *q     = xjs_ctx_of(c);
  size_t     bytes = xjs_utf16_to_utf8(name->data, name->length, NULL, 0);
  char      *buf   = (char *)malloc(bytes + 1);
  if (!buf) return false;
  xjs_utf16_to_utf8(name->data, name->length, buf, bytes);
  buf[bytes]  = 0;
  JSAtom atom = JS_NewAtom(q, buf);
  int    r    = JS_DeleteProperty(q, xjs_slot_qv((xJSValueRef)o), atom, 0);
  JS_FreeAtom(q, atom);
  free(buf);
  if (r < 0) {
    xjs_propagate_exception(q, exc);
    return false;
  }
  return r == 1;
}

xJSValueRef xJSObjectGetPropertyAtIndex(xJSContextRef c, xJSObjectRef o,
                                        unsigned idx, xJSValueRef *exc) {
  if (!o) return NULL;
  JSContext *q = xjs_ctx_of(c);
  JSValue    v = JS_GetPropertyUint32(q, xjs_slot_qv((xJSValueRef)o), idx);
  if (JS_IsException(v)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return xjs_slot_make(q, v);
}

void xJSObjectSetPropertyAtIndex(xJSContextRef c, xJSObjectRef o, unsigned idx,
                                 xJSValueRef v, xJSValueRef *exc) {
  if (!o) return;
  JSContext *q  = xjs_ctx_of(c);
  JSValue    qv = v ? JS_DupValue(q, xjs_slot_qv(v)) : JS_UNDEFINED;
  if (JS_SetPropertyUint32(q, xjs_slot_qv((xJSValueRef)o), idx, qv) < 0) {
    xjs_propagate_exception(q, exc);
  }
}

void *xJSObjectGetPrivate(xJSObjectRef o) {
  if (!o) return NULL;
  JSValue   qv = xjs_slot_qv((xJSValueRef)o);
  JSClassID id = JS_GetClassID(qv);
  if (id == JS_INVALID_CLASS_ID) return NULL;
  struct xjs_native_priv *priv =
    (struct xjs_native_priv *)JS_GetOpaque(qv, id);
  return priv ? priv->data : NULL;
}

bool xJSObjectSetPrivate(xJSObjectRef o, void *data) {
  if (!o) return false;
  JSValue   qv = xjs_slot_qv((xJSValueRef)o);
  JSClassID id = JS_GetClassID(qv);
  if (id == JS_INVALID_CLASS_ID) return false;
  struct xjs_native_priv *priv =
    (struct xjs_native_priv *)JS_GetOpaque(qv, id);
  if (!priv) return false;
  priv->data = data;
  return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Object — call / construct
 * ═══════════════════════════════════════════════════════════════════ */

bool xJSObjectIsFunction(xJSContextRef c, xJSObjectRef o) {
  return o ? JS_IsFunction(xjs_ctx_of(c), xjs_slot_qv((xJSValueRef)o)) == 1
           : false;
}

xJSValueRef xJSObjectCallAsFunction(xJSContextRef c, xJSObjectRef o,
                                    xJSObjectRef thisObj, size_t argc,
                                    const xJSValueRef argv[],
                                    xJSValueRef      *exc) {
  if (!o) return NULL;
  JSContext *q     = xjs_ctx_of(c);
  JSValue    thisV = thisObj ? xjs_slot_qv((xJSValueRef)thisObj) : JS_UNDEFINED;
  JSValue   *qargs = NULL;
  if (argc) {
    qargs = (JSValue *)malloc(argc * sizeof(JSValue));
    if (!qargs) return NULL;
    for (size_t i = 0; i < argc; ++i)
      qargs[i] = xjs_slot_qv(argv[i]);
  }
  JSValue r = JS_Call(q, xjs_slot_qv((xJSValueRef)o), thisV, (int)argc, qargs);
  free(qargs);
  if (JS_IsException(r)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return xjs_slot_make(q, r);
}

bool xJSObjectIsConstructor(xJSContextRef c, xJSObjectRef o) {
  return o ? JS_IsConstructor(xjs_ctx_of(c), xjs_slot_qv((xJSValueRef)o)) == 1
           : false;
}

xJSObjectRef xJSObjectCallAsConstructor(xJSContextRef c, xJSObjectRef o,
                                        size_t argc, const xJSValueRef argv[],
                                        xJSValueRef *exc) {
  if (!o) return NULL;
  JSContext *q     = xjs_ctx_of(c);
  JSValue   *qargs = NULL;
  if (argc) {
    qargs = (JSValue *)malloc(argc * sizeof(JSValue));
    if (!qargs) return NULL;
    for (size_t i = 0; i < argc; ++i)
      qargs[i] = xjs_slot_qv(argv[i]);
  }
  JSValue r =
    JS_CallConstructor(q, xjs_slot_qv((xJSValueRef)o), (int)argc, qargs);
  free(qargs);
  if (JS_IsException(r)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  return (xJSObjectRef)xjs_slot_make(q, r);
}

/* ═══════════════════════════════════════════════════════════════════
 * Property-name array / accumulator
 * ═══════════════════════════════════════════════════════════════════ */

xJSPropertyNameArrayRef xJSObjectCopyPropertyNames(xJSContextRef c,
                                                   xJSObjectRef  o) {
  if (!o) return NULL;
  JSContext       *q   = xjs_ctx_of(c);
  JSPropertyEnum  *tab = NULL;
  uint32_t         len = 0;

  /* JSC semantics: own, enumerable, string-keyed properties only —
   * mirrors Object.keys().  We deliberately omit JS_GPN_SYMBOL_MASK
   * and JS_GPN_PRIVATE_MASK so the result is stable across QuickJS
   * versions that added symbol visibility later. */
  if (JS_GetOwnPropertyNames(q, &tab, &len, xjs_slot_qv((xJSValueRef)o),
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    /* GetOwnPropertyNames only fails on allocation failure or on a
     * throwing Proxy trap; swallow the pending exception — the public
     * signature has no out-exc parameter. */
    JSValue ex = JS_GetException(q);
    JS_FreeValue(q, ex);
    return NULL;
  }

  struct OpaqueXJSPropertyNameArray *a =
    (struct OpaqueXJSPropertyNameArray *)calloc(
      1, sizeof(struct OpaqueXJSPropertyNameArray));
  if (!a) goto fail_tab;
  a->refcount = 1;
  a->count    = 0;
  a->names    = NULL;
  if (len) {
    a->names = (xJSStringRef *)calloc(len, sizeof(xJSStringRef));
    if (!a->names) goto fail_array;
  }

  for (uint32_t i = 0; i < len; ++i) {
    const char *s = JS_AtomToCString(q, tab[i].atom);
    if (!s) goto fail_partial;
    xJSStringRef str = xJSStringCreateWithUTF8CString(s);
    JS_FreeCString(q, s);
    if (!str) goto fail_partial;
    a->names[a->count++] = str; /* retained by Create */
  }

  /* Release the QuickJS property-enum table. */
  for (uint32_t i = 0; i < len; ++i) JS_FreeAtom(q, tab[i].atom);
  js_free(q, tab);
  return a;

fail_partial:
  for (size_t i = 0; i < a->count; ++i) xJSStringRelease(a->names[i]);
  free(a->names);
fail_array:
  free(a);
fail_tab:
  for (uint32_t i = 0; i < len; ++i) JS_FreeAtom(q, tab[i].atom);
  js_free(q, tab);
  return NULL;
}

xJSPropertyNameArrayRef xJSPropertyNameArrayRetain(xJSPropertyNameArrayRef a) {
  if (a) a->refcount++;
  return a;
}

void xJSPropertyNameArrayRelease(xJSPropertyNameArrayRef a) {
  if (!a) return;
  if (--a->refcount > 0) return;
  for (size_t i = 0; i < a->count; ++i)
    xJSStringRelease(a->names[i]);
  free(a->names);
  free(a);
}

size_t xJSPropertyNameArrayGetCount(xJSPropertyNameArrayRef a) {
  return a ? a->count : 0;
}

xJSStringRef xJSPropertyNameArrayGetNameAtIndex(xJSPropertyNameArrayRef a,
                                                size_t                  i) {
  return (a && i < a->count) ? a->names[i] : NULL;
}

void xJSPropertyNameAccumulatorAddName(xJSPropertyNameAccumulatorRef acc,
                                       xJSStringRef                  name) {
  if (!acc || !name) return;
  if (acc->count == acc->capacity) {
    size_t        nc = acc->capacity ? acc->capacity * 2 : 8;
    xJSStringRef *nn = (xJSStringRef *)realloc(acc->names, nc * sizeof(*nn));
    if (!nn) return;
    acc->names    = nn;
    acc->capacity = nc;
  }
  acc->names[acc->count++] = xJSStringRetain(name);
}
