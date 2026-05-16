/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_class.c - xjs class registration.
 *
 * A JSC-shaped xJSClass maps onto QuickJS's (JSClassID + JSClassDef)
 * pair.  Because xJSClassCreate() has no JSRuntime in hand — callers
 * build classes at module-init time, well before any context exists —
 * we defer BOTH the JSClassID allocation and the JS_NewClass
 * registration to first use, on a per-runtime basis via
 * xjs_class_ensure_registered().  (quickjs-ng made class ids
 * per-runtime, so JS_NewClassID must be called with a real rt.)
 *
 * Native objects created with xJSObjectMake(ctx, cls, data) carry an
 * xjs_native_priv allocated on the C heap and installed via
 * JS_SetOpaque.  The only QuickJS-level finalizer we register is
 * xjs_class_finalize(), which (a) invokes the user-provided
 * def.finalize, and (b) frees the xjs_native_priv itself.
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* ═══════════════════════════════════════════════════════════════════
 * Finalize trampoline
 *
 * QuickJS invokes finalizers during GC with only a JSRuntime (no
 * JSContext), so we cannot box `val` into an xJSValueRef slot the way
 * the normal API does.  That matches JSC's own finalize contract:
 * the object pointer passed to the user callback is only valid for
 * reading the private-data slot, not for any context-dependent
 * operation.  We therefore synthesise a short-lived stack slot for
 * the duration of the callback; xJSObjectGetPrivate sees it through
 * the tagged-pointer machinery and returns priv->data.
 * ═══════════════════════════════════════════════════════════════════ */

static void xjs_class_finalize(JSRuntime *rt, JSValue val) {
  JSClassID id = JS_GetClassID(val);
  if (id == JS_INVALID_CLASS_ID) return;

  struct xjs_native_priv *priv =
    (struct xjs_native_priv *)JS_GetOpaque(val, id);
  if (!priv) return;

  if (priv->jsclass && priv->jsclass->def.finalize) {
    /* Stack-allocated transient slot: refcount 0 (never freed via
     * the slot arena), ctx NULL (unavailable at GC time), qv copies
     * val without ref-bumping.  JSC users only read private data
     * from finalize, which xJSObjectGetPrivate satisfies via the
     * opaque attached above. */
    struct OpaqueXJSValue stub;
    stub.refcount = 0;
    stub.ctx      = NULL;
    stub.qv       = val;
    priv->jsclass->def.finalize((xJSObjectRef)&stub);
  }
  /* The class itself may long outlive individual instances; we do
   * not release it here (private creation path did not retain). */
  free(priv);
  (void)rt;
}

/* ═══════════════════════════════════════════════════════════════════
 * Lazy registration
 * ═══════════════════════════════════════════════════════════════════ */

int xjs_class_ensure_registered(xJSClassRef cls, JSRuntime *rt) {
  if (!cls || !rt) return -1;
  /* Lazy classid allocation: we could not do this in xJSClassCreate
   * because class ids are per-runtime in quickjs-ng.  The first runtime
   * to ensure this class also imprints its id on the class; subsequent
   * runtimes would see a non-zero id and re-register under the same
   * number, which is the intended cross-runtime sharing semantics for
   * JSC-shaped class handles. */
  if (cls->qclass == 0) JS_NewClassID(rt, &cls->qclass);
  if (JS_IsRegisteredClass(rt, cls->qclass)) return 0;

  JSClassDef def;
  memset(&def, 0, sizeof(def));
  def.class_name = cls->class_name ? cls->class_name : "";
  def.finalizer  = xjs_class_finalize;
  def.gc_mark    = NULL;
  def.call       = NULL;
  def.exotic     = NULL;
  return JS_NewClass(rt, cls->qclass, &def) == 0 ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

xJSClassRef xJSClassCreate(const xJSClassDefinition *def) {
  if (!def) return NULL;

  struct OpaqueXJSClass *k =
    (struct OpaqueXJSClass *)calloc(1, sizeof(*k));
  if (!k) return NULL;

  k->refcount = 1;
  k->def      = *def; /* shallow copy: string/table pointers aliased */

  /* Duplicate className so QuickJS holds a stable pointer even if the
   * caller's JSClassDefinition lives on the stack. */
  if (def->className) {
    k->class_name = strdup(def->className);
    if (!k->class_name) {
      free(k);
      return NULL;
    }
  }

  /* JSClassID allocation is deferred to xjs_class_ensure_registered —
   * quickjs-ng requires a JSRuntime* to allocate class ids, which we
   * do not have here.  qclass==0 is our "unregistered" sentinel. */
  k->qclass = 0;

  return k;
}

xJSClassRef xJSClassRetain(xJSClassRef k) {
  if (k) k->refcount++;
  return k;
}

void xJSClassRelease(xJSClassRef k) {
  if (!k) return;
  if (--k->refcount > 0) return;
  free(k->class_name);
  free(k);
}
