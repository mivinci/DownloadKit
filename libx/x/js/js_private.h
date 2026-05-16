/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_private.h - xjs internal types and helpers (not installed).
 *
 * The public xjs API is a JSC-shaped facade over QuickJS.  QuickJS
 * types live only inside this header and the .c files that include
 * it; public js.h never sees them.
 *
 * This header also declares the cross-TU helpers (slot refcount
 * plumbing, UTF-8/UTF-16 conversion, exception propagation) that
 * the js_*.c implementation files share.  Nothing declared here is
 * part of the public API.
 *
 * Value representation
 * --------------------
 * xJSValueRef is a *tagged pointer*, not a plain heap pointer.  The
 * low 3 bits of the uintptr_t encode one of:
 *
 *   TAG_HEAP (000)     - pointer to a pool-allocated OpaqueXJSValue
 *                        that owns one JS_DupValue reference.
 *   TAG_INT31 (001)    - inline 31-bit signed integer, no allocation.
 *                        Payload lives in the high 32 bits.
 *   TAG_SINGLETON (010)- one of 4 immortal constants (undefined, null,
 *                        true, false) encoded in the payload bits.
 *
 * A NULL xJSValueRef means "no value" (e.g. an unset exception out-
 * parameter); it is distinct from undefined.  Only HEAP-tagged values
 * carry a refcount; retain/release on inline values are no-ops.
 *
 * Heap slots are carved from a per-context *chunk arena*: each
 * OpaqueXJSContext owns a linked list of geometrically-growing chunks
 * of OpaqueXJSValue storage.  Allocation is O(1) bump within the
 * newest chunk; a freelist threaded through released slots lets us
 * recycle memory without fragmenting the arena.  Chunk memory is
 * freed in one shot when the context is released.
 *
 * Reference-counting strategy
 * ---------------------------
 *   - ContextGroup / GlobalContext / Class / String / PropertyNameArray
 *     are heap objects with an int refcount, created with refcount = 1.
 *   - Heap value slots carry an int refcount and hold one JS_DupValue
 *     reference on their JSValue.  xJSValueProtect/Unprotect adjust it.
 *   - Inline (INT31 / SINGLETON) xJSValueRefs are conceptually
 *     immortal: they have no allocation and no refcount.
 */

#ifndef XJS_JS_PRIVATE_H
#define XJS_JS_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/event.h>
#include <x/base/memory.h>
#include <x/base/slab.h>

#include "js.h"

/* Tests that exercise private invariants need access to the
 * OpaqueXJS* structs but must not pull in quickjs.h — its source
 * directory ships a bare file named `version` that shadows the
 * libc++ `<version>` standard header.  Test targets define
 * XJS_TEST_NO_QUICKJS to substitute a minimal forward-declaration
 * of the QuickJS types used in our struct layouts.  The JSValue
 * placeholder only needs to be at least as large as the real
 * JSValue so that any fields after it stay at stable offsets —
 * tests never access `.qv` directly. */
#ifdef XJS_TEST_NO_QUICKJS
typedef struct JSContext JSContext;
typedef struct JSRuntime JSRuntime;
typedef uint32_t         JSClassID;
typedef struct {
  uint64_t _opaque[2];
} JSValue;
#else
#include "quickjs.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * xJSValueRef tagged-pointer encoding
 * ═══════════════════════════════════════════════════════════════════ */

#define XJS_TAG_MASK      ((uintptr_t)0x7)
#define XJS_TAG_HEAP      ((uintptr_t)0x0) /* pointer (8-byte aligned)   */
#define XJS_TAG_INT31     ((uintptr_t)0x1) /* high 32 bits = int32_t     */
#define XJS_TAG_SINGLETON ((uintptr_t)0x2) /* 4 immortal constants       */

/* Singleton payload: bits 3..4 distinguish undefined / null / false / true. */
#define XJS_SINGLETON_UNDEFINED ((uintptr_t)((0u << 3) | XJS_TAG_SINGLETON))
#define XJS_SINGLETON_NULL      ((uintptr_t)((1u << 3) | XJS_TAG_SINGLETON))
#define XJS_SINGLETON_FALSE     ((uintptr_t)((2u << 3) | XJS_TAG_SINGLETON))
#define XJS_SINGLETON_TRUE      ((uintptr_t)((3u << 3) | XJS_TAG_SINGLETON))

/* int31 range: [-2^30, 2^30).  Values outside this range fall back to
 * heap-slot float64 storage. */
#define XJS_INT31_MIN (-(int32_t)(1 << 30))
#define XJS_INT31_MAX ((int32_t)(1 << 30) - 1)

static inline int xjs_tag(xJSValueRef v) {
  return (int)((uintptr_t)v & XJS_TAG_MASK);
}
static inline int xjs_is_heap(xJSValueRef v) {
  return v != NULL && xjs_tag(v) == (int)XJS_TAG_HEAP;
}
static inline int xjs_is_int31(xJSValueRef v) {
  return xjs_tag(v) == (int)XJS_TAG_INT31;
}
static inline int xjs_is_singleton(xJSValueRef v) {
  return xjs_tag(v) == (int)XJS_TAG_SINGLETON;
}
static inline int32_t xjs_int31_get(xJSValueRef v) {
  /* arithmetic shift preserves sign */
  return (int32_t)((intptr_t)v >> 32);
}
static inline xJSValueRef xjs_int31_make(int32_t i) {
  return (xJSValueRef)(((uintptr_t)(uint32_t)i << 32) | XJS_TAG_INT31);
}

/* ─── Context group (≈ QuickJS JSRuntime) ───────────────────────── */
struct OpaqueXJSContextGroup {
  int        refcount;
  JSRuntime *rt;
};

/* ─── Global context (≈ QuickJS JSContext) ──────────────────────── */
struct OpaqueXJSContext {
  int                refcount;
  JSContext         *ctx;
  xJSContextGroupRef group; /* retained */
  char              *name;  /* optional label, heap-owned */
  /* Slot pool: fixed-size object pool backing the tagged-pointer
   * heap slots (OpaqueXJSValue).  A JSContext is single-threaded by
   * construction — all slot allocation / release happens on the
   * thread that drives the context — so we use the cheaper xSlab
   * (no locking) rather than xSlabMt.  The pool is freed in one
   * shot when the context is released; any still-live slots leak
   * their cells, which is acceptable at context teardown. */
  xSlab *slot_pool;
  /* ES module loader.  QuickJS attaches a loader per-runtime, not
   * per-context; we route through a trampoline that recovers the
   * owning xjs context via JS_GetContextOpaque() and invokes the
   * user callback stored here.  NULL when no loader is installed;
   * in that case every `import` rejects with ReferenceError. */
  xJSModuleLoadCallback module_load_cb;
  void                 *module_load_opaque;
};

/* ─── Value slot (boxes a QuickJS JSValue) ──────────────────────── */
/* Only allocated for non-inline values.  Sized/aligned so that
 * (uintptr_t)&slot has low 3 bits = 0 (TAG_HEAP). */
struct OpaqueXJSValue {
  int        refcount;
  JSContext *ctx; /* non-owning; the context outlives all its slots */
  JSValue    qv;  /* owns one JS_DupValue reference */
};

/* ─── String (UTF-16 buffer, independent lifetime) ──────────────── */
struct OpaqueXJSString {
  int       refcount;
  uint16_t *data;   /* NUL-terminated UTF-16 */
  size_t    length; /* in code units, excluding NUL */
};

/* ─── Class (wraps a QuickJS JSClassID + the definition) ────────── */
/*
 * Classes are created eagerly (xJSClassCreate allocates a JSClassID
 * via JS_NewClassID), but the actual JS_NewClass registration against
 * a JSRuntime is deferred to first use — xJSClassCreate has no
 * runtime yet.  xjs_class_ensure_registered() performs the lazy
 * registration and is idempotent across contexts that share a
 * runtime.
 *
 * `def` is a shallow copy of the caller's xJSClassDefinition.  The
 * C strings it points at (className, staticFunctions[i].name, ...)
 * are assumed by the JSC contract to outlive the class; we rely on
 * that and do not duplicate them, except for className which we
 * copy into `class_name` so QuickJS has a stable pointer.
 */
struct OpaqueXJSClass {
  int                refcount;
  JSClassID          qclass;
  xJSClassDefinition def;        /* shallow-copied */
  char              *class_name; /* owned; stable pointer for QuickJS */
};

/* Per-native-object slot attached via JS_SetOpaque.  Holds the
 * xjs-level private data pointer and a back-pointer to the class so
 * the finalize trampoline can recover the user callback without
 * needing the JSContext (which QuickJS does not provide at GC time). */
struct xjs_native_priv {
  xJSClassRef jsclass; /* non-retaining back-pointer */
  void       *data;
};

/* ─── Property-name array (snapshot of object keys) ─────────────── */
struct OpaqueXJSPropertyNameArray {
  int           refcount;
  xJSStringRef *names; /* each retained */
  size_t        count;
};

/* ─── Property-name accumulator (used during getPropertyNames cb) ─ */
struct OpaqueXJSPropertyNameAccumulator {
  xJSStringRef *names;
  size_t        count;
  size_t        capacity;
};

/* ═══════════════════════════════════════════════════════════════════
 * Cross-TU helpers (defined in js_private.c)
 * ═══════════════════════════════════════════════════════════════════ */

/* ─── Context helpers ─────────────────────────────────────────────── */

/* Borrow the underlying QuickJS JSContext* from an xJSContextRef. */
JSContext *xjs_ctx_of(xJSContextRef ctx);

/* Non-const view onto OpaqueXJSContext.  xJSContextRef is typed
 * `const struct OpaqueXJSContext *` to match JSC; we freely cast
 * away const when we need to mutate refcount/name. */
struct OpaqueXJSContext *xjs_ctx_mut(xJSContextRef ctx);

/* Recover the owning OpaqueXJSContext* from a QuickJS JSContext*.
 * Relies on JS_SetContextOpaque being called at creation time. */
struct OpaqueXJSContext *xjs_ctx_from_q(JSContext *qctx);

/* ─── Slot-pool lifecycle (called from js_context.c) ──────────────── */

/* Initialise the per-context slot pool.  Returns 0 on success, -1 on
 * OOM.  On failure the context's slot_pool stays NULL and the caller
 * must not proceed to use the context. */
int  xjs_slot_arena_init(struct OpaqueXJSContext *c);
void xjs_slot_arena_destroy(struct OpaqueXJSContext *c);

/* ─── Value-slot plumbing ─────────────────────────────────────────── */

/* Box a QuickJS JSValue into an xJSValueRef.  Takes ownership of one
 * reference on @p qv.  If @p qv is JS_EXCEPTION we free it and return
 * NULL.  Inline-eligible values (undefined/null/bool/small-int) are
 * returned as tagged pointers with no allocation; everything else
 * gets a heap slot with refcount 1 drawn from the context arena. */
xJSValueRef xjs_slot_make(JSContext *qctx, JSValue qv);

/* Take an additional reference.  No-op for inline (non-heap) values. */
void xjs_slot_retain(xJSValueRef v);

/* Drop a reference; free when the last one goes.  No-op for inline. */
void xjs_slot_release(xJSValueRef v);

/* Borrow a QuickJS JSValue for read-only use.  For heap slots returns
 * the stored JSValue; for inline values synthesises the equivalent
 * JSValue.  The returned value is borrowed — callers must not
 * JS_FreeValue it. */
JSValue xjs_slot_qv(xJSValueRef v);

/* On an API boundary where QuickJS may have set a pending exception,
 * drain it into the caller-supplied out-param (if any). */
void xjs_propagate_exception(JSContext *qctx, xJSValueRef *exception);

/* ─── UTF-8 / UTF-16 conversion ───────────────────────────────────── */

/* Minimal UTF-8 → UTF-16 decoder (BMP + surrogates).  Returns number
 * of UTF-16 code units needed (or written when dst != NULL). */
size_t xjs_utf8_to_utf16(const char *src, size_t srclen, uint16_t *dst, size_t dstcap);

/* UTF-16 → UTF-8.  Returns number of bytes written (not counting NUL)
 * when dst is non-NULL; total bytes needed otherwise. */
size_t xjs_utf16_to_utf8(const uint16_t *src, size_t srclen, char *dst, size_t dstcap);

/* Build a freshly-owned QuickJS string JSValue from an xJSStringRef. */
JSValue xjs_qv_from_string(JSContext *qctx, xJSStringRef s);

/* ─── Class registration helpers ──────────────────────────────────── */

/* Ensure @p cls is registered against @p rt (JS_NewClass).  Idempotent
 * across runtimes: the same xJSClassRef can be used from multiple
 * context groups.  Returns 0 on success, -1 on failure. */
int xjs_class_ensure_registered(xJSClassRef cls, JSRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif /* XJS_JS_PRIVATE_H */
