/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_context.c - xjs context-group and global-context lifecycle.
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__) || defined(__ANDROID__)
#include <malloc.h>
#endif

#include "quickjs.h"

/* Module loader trampoline lives in js_module.c — forward declare so
 * we can plug it into the runtime at context creation. */
JSModuleDef *xjs_module_loader_trampoline(JSContext *ctx,
                                          const char *module_name, void *opaque,
                                          JSValueConst attributes);

/* ═══════════════════════════════════════════════════════════════════
 * Allocator bridging (QuickJS expects plain malloc/free/realloc)
 * ═══════════════════════════════════════════════════════════════════ */

static void *xjs_mf_malloc(void *opaque, size_t size) {
  (void)opaque;
  return size ? malloc(size) : NULL;
}
static void *xjs_mf_calloc(void *opaque, size_t count, size_t size) {
  (void)opaque;
  return (count && size) ? calloc(count, size) : NULL;
}
static void xjs_mf_free(void *opaque, void *ptr) {
  (void)opaque;
  free(ptr);
}
static void *xjs_mf_realloc(void *opaque, void *ptr, size_t size) {
  (void)opaque;
  return realloc(ptr, size);
}
static size_t xjs_mf_usable_size(const void *ptr) {
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(__GLIBC__) || defined(__ANDROID__)
  return malloc_usable_size((void *)ptr);
#else
  /* Fallback: lying here only hurts the GC growth heuristic, not
   * correctness.  quickjs-ng ships the same fallback upstream. */
  (void)ptr;
  return 0;
#endif
}

static const JSMallocFunctions kXJSMallocFns = {
  .js_calloc             = xjs_mf_calloc,
  .js_malloc             = xjs_mf_malloc,
  .js_free               = xjs_mf_free,
  .js_realloc            = xjs_mf_realloc,
  .js_malloc_usable_size = xjs_mf_usable_size,
};

/* ═══════════════════════════════════════════════════════════════════
 * Context group
 * ═══════════════════════════════════════════════════════════════════ */

xJSContextGroupRef xJSContextGroupCreate(void) {
  struct OpaqueXJSContextGroup *g =
    (struct OpaqueXJSContextGroup *)calloc(1, sizeof(*g));
  if (!g) return NULL;
  g->refcount = 1;
  g->rt       = JS_NewRuntime2(&kXJSMallocFns, NULL);
  if (!g->rt) {
    free(g);
    return NULL;
  }
  /* Stash the group back-pointer so callbacks invoked by QuickJS can
   * find us via the runtime opaque. */
  JS_SetRuntimeOpaque(g->rt, g);
  /* Install our module-loader trampoline unconditionally.  With no
   * user loader set on a context, the trampoline rejects every
   * import with a ReferenceError — same behaviour as before, except
   * reachable now through the ES module machinery.  We pass a NULL
   * normalize func so QuickJS's default "./x relative to importer"
   * resolution handles specifier normalisation for us. */
  JS_SetModuleLoaderFunc2(g->rt, /*module_normalize*/ NULL,
                          xjs_module_loader_trampoline,
                          /*check_attrs*/ NULL, /*opaque*/ NULL);
  return g;
}

xJSContextGroupRef xJSContextGroupRetain(xJSContextGroupRef g) {
  if (g) g->refcount++;
  return g;
}

void xJSContextGroupRelease(xJSContextGroupRef g) {
  if (!g) return;
  if (--g->refcount > 0) return;
  JS_FreeRuntime(g->rt);
  free(g);
}

/* ═══════════════════════════════════════════════════════════════════
 * Global context
 * ═══════════════════════════════════════════════════════════════════ */

xJSGlobalContextRef
xJSGlobalContextCreateInGroup(xJSContextGroupRef group,
                              xJSClassRef        globalObjectClass) {
  (void)globalObjectClass; /* [TODO] custom global not implemented */
  if (!group) return NULL;
  struct OpaqueXJSContext *c = (struct OpaqueXJSContext *)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->refcount = 1;
  c->ctx      = JS_NewContext(group->rt);
  if (!c->ctx) {
    free(c);
    return NULL;
  }
  if (xjs_slot_arena_init(c) != 0) {
    JS_FreeContext(c->ctx);
    free(c);
    return NULL;
  }
  c->group = xJSContextGroupRetain(group);
  JS_SetContextOpaque(c->ctx, c);
  return c;
}

xJSGlobalContextRef xJSGlobalContextCreate(xJSClassRef globalObjectClass) {
  xJSContextGroupRef g = xJSContextGroupCreate();
  if (!g) return NULL;
  xJSGlobalContextRef c = xJSGlobalContextCreateInGroup(g, globalObjectClass);
  /* context now retains the group; drop our local ref */
  xJSContextGroupRelease(g);
  return c;
}

xJSGlobalContextRef xJSGlobalContextRetain(xJSGlobalContextRef ctx) {
  if (ctx) ctx->refcount++;
  return ctx;
}

void xJSGlobalContextRelease(xJSGlobalContextRef ctx) {
  if (!ctx) return;
  if (--ctx->refcount > 0) return;
  /* JSContext release will JS_FreeValue any values the pool still
   * references through QuickJS's own bookkeeping.  We then tear down
   * our own slot pool: any still-live slots leak their heap memory
   * here, which is acceptable since the whole context is going away. */
  JS_FreeContext(ctx->ctx);
  xjs_slot_arena_destroy(ctx);
  xJSContextGroupRelease(ctx->group);
  free(ctx->name);
  free(ctx);
}

xJSObjectRef xJSContextGetGlobalObject(xJSContextRef ctx) {
  JSContext *q = xjs_ctx_of(ctx);
  if (!q) return NULL;
  JSValue g = JS_GetGlobalObject(q);
  return (xJSObjectRef)xjs_slot_make(q, g);
}

xJSContextGroupRef xJSContextGetGroup(xJSContextRef ctx) {
  return ctx ? ctx->group : NULL;
}

xJSGlobalContextRef xJSContextGetGlobalContext(xJSContextRef ctx) {
  return xjs_ctx_mut(ctx); /* xjs only supports global contexts currently */
}

xJSStringRef xJSGlobalContextCopyName(xJSGlobalContextRef ctx) {
  if (!ctx || !ctx->name) return NULL;
  return xJSStringCreateWithUTF8CString(ctx->name);
}

void xJSGlobalContextSetName(xJSGlobalContextRef ctx, xJSStringRef name) {
  if (!ctx) return;
  free(ctx->name);
  ctx->name = NULL;
  if (!name) return;
  size_t sz = xJSStringGetMaximumUTF8CStringSize(name);
  ctx->name = (char *)malloc(sz);
  if (ctx->name) xJSStringGetUTF8CString(name, ctx->name, sz);
}

void xJSContextSetModuleLoader(xJSGlobalContextRef   ctx,
                               xJSModuleLoadCallback load, void *opaque) {
  if (!ctx) return;
  ctx->module_load_cb     = load;
  ctx->module_load_opaque = opaque;
}
