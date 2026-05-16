/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_private.c - shared helpers (see js_private.h).
 */

#include "js_private.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Context helpers
 * ═══════════════════════════════════════════════════════════════════ */

JSContext *xjs_ctx_of(xJSContextRef ctx) {
  return ctx ? ((struct OpaqueXJSContext *)ctx)->ctx : NULL;
}

struct OpaqueXJSContext *xjs_ctx_mut(xJSContextRef ctx) {
  return (struct OpaqueXJSContext *)ctx;
}

struct OpaqueXJSContext *xjs_ctx_from_q(JSContext *qctx) {
  return qctx ? (struct OpaqueXJSContext *)JS_GetContextOpaque(qctx) : NULL;
}

/* ══════════════════════════════════════════════════════════════════
 * Slot pool: fixed-size object pool backed by xSlab.
 *
 * Each heap-tagged xJSValueRef points at an OpaqueXJSValue carved
 * out of this pool.  xSlab's default 16-byte alignment satisfies
 * the tagged-pointer invariant (low 3 bits must be zero).  The
 * JSContext is single-threaded, so no locking is needed — we use
 * xSlab, not xSlabMt.
 *
 * The pool is destroyed in one shot when the context is released;
 * any still-live slots leak their cells, which is acceptable at
 * teardown time (the whole runtime is going away).
 * ═════════════════════════════════════════════════════════════════ */

int xjs_slot_arena_init(struct OpaqueXJSContext *c) {
  /* align=0 → xSlab's 16-byte default, which keeps the low 3 bits
   * zero as required by the tagged-pointer encoding.
   * chunk_bytes=0 → the 64 KiB default, sized so each chunk holds
   * ~2K slots — well within the geometric range (16..4096) the
   * previous hand-rolled arena used. */
  c->slot_pool = xSlabCreate(sizeof(struct OpaqueXJSValue), 0, 0);
  return c->slot_pool ? 0 : -1;
}

void xjs_slot_arena_destroy(struct OpaqueXJSContext *c) {
  xSlabDestroy(c->slot_pool);
  c->slot_pool = NULL;
}
/* ═══════════════════════════════════════════════════════════════════
 * Value-slot plumbing (tagged-pointer aware)
 * ═══════════════════════════════════════════════════════════════════ */

xJSValueRef xjs_slot_make(JSContext *qctx, JSValue qv) {
  /* Exceptions never become slots; the caller will propagate them. */
  if (JS_IsException(qv)) {
    JS_FreeValue(qctx, qv);
    return NULL;
  }

  /* Inline fast paths: no allocation, no refcount. */
  int tag = JS_VALUE_GET_TAG(qv);
  if (tag == JS_TAG_UNDEFINED) return (xJSValueRef)XJS_SINGLETON_UNDEFINED;
  if (tag == JS_TAG_NULL) return (xJSValueRef)XJS_SINGLETON_NULL;
  if (tag == JS_TAG_BOOL) {
    return (xJSValueRef)(JS_VALUE_GET_BOOL(qv) ? XJS_SINGLETON_TRUE
                                               : XJS_SINGLETON_FALSE);
  }
  if (tag == JS_TAG_INT) {
    int32_t i = JS_VALUE_GET_INT(qv);
    if (i >= XJS_INT31_MIN && i <= XJS_INT31_MAX) {
      return xjs_int31_make(i);
    }
    /* Out-of-range ints fall through to heap-slot storage. */
  }

  /* Heap slot from the owning context's pool. */
  struct OpaqueXJSContext *owner = xjs_ctx_from_q(qctx);
  if (!owner) {
    /* Should never happen: every context we create calls
     * JS_SetContextOpaque.  Be defensive and drop the value. */
    JS_FreeValue(qctx, qv);
    return NULL;
  }
  struct OpaqueXJSValue *s =
    (struct OpaqueXJSValue *)xSlabAlloc(owner->slot_pool);
  if (!s) {
    JS_FreeValue(qctx, qv);
    return NULL;
  }
  s->refcount = 1;
  s->ctx      = qctx;
  s->qv       = qv;
  return (xJSValueRef)s;
}

void xjs_slot_retain(xJSValueRef v) {
  if (!xjs_is_heap(v)) return; /* NULL and inline: no-op */
  ((struct OpaqueXJSValue *)v)->refcount++;
}

void xjs_slot_release(xJSValueRef v) {
  if (!xjs_is_heap(v)) return; /* NULL and inline: no-op */
  struct OpaqueXJSValue *s = (struct OpaqueXJSValue *)v;
  if (--s->refcount > 0) return;
  JS_FreeValue(s->ctx, s->qv);
  struct OpaqueXJSContext *owner = xjs_ctx_from_q(s->ctx);
  if (owner) xSlabFree(owner->slot_pool, s);
  /* If owner is gone we simply leak the cell; the pool is about to
   * be torn down anyway. */
}

JSValue xjs_slot_qv(xJSValueRef v) {
  if (!v) return JS_UNDEFINED;
  switch (xjs_tag(v)) {
  case (int)XJS_TAG_HEAP:
    return ((struct OpaqueXJSValue *)v)->qv;
  case (int)XJS_TAG_INT31:
    return JS_MKVAL(JS_TAG_INT, xjs_int31_get(v));
  case (int)XJS_TAG_SINGLETON:
    if ((uintptr_t)v == XJS_SINGLETON_UNDEFINED) return JS_UNDEFINED;
    if ((uintptr_t)v == XJS_SINGLETON_NULL) return JS_NULL;
    return (uintptr_t)v == XJS_SINGLETON_TRUE ? JS_TRUE : JS_FALSE;
  default:
    return JS_UNDEFINED; /* unreachable */
  }
}

void xjs_propagate_exception(JSContext *qctx, xJSValueRef *exception) {
  JSValue exc = JS_GetException(qctx);
  if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
    JS_FreeValue(qctx, exc);
    return;
  }
  if (exception) {
    *exception = xjs_slot_make(qctx, exc);
  } else {
    JS_FreeValue(qctx, exc);
  }
}

/* ─── UTF-8 / UTF-16 conversion ───────────────────────────────────── */

size_t xjs_utf8_to_utf16(const char *src, size_t srclen, uint16_t *dst,
                         size_t dstcap) {
  size_t i = 0, o = 0;
  while (i < srclen) {
    unsigned char c = (unsigned char)src[i];
    uint32_t      cp;
    size_t        n;
    if (c < 0x80) {
      cp = c;
      n  = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n  = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n  = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n  = 4;
    } else {
      cp = 0xFFFD;
      n  = 1;
    }
    if (i + n > srclen) {
      cp = 0xFFFD;
      n  = srclen - i;
    }
    for (size_t k = 1; k < n; ++k) {
      unsigned char cc = (unsigned char)src[i + k];
      if ((cc & 0xC0) != 0x80) {
        cp = 0xFFFD;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += n;
    if (cp < 0x10000) {
      if (dst && o < dstcap) dst[o] = (uint16_t)cp;
      o++;
    } else {
      cp -= 0x10000;
      if (dst && o + 1 < dstcap) {
        dst[o]     = (uint16_t)(0xD800 | (cp >> 10));
        dst[o + 1] = (uint16_t)(0xDC00 | (cp & 0x3FF));
      }
      o += 2;
    }
  }
  return o;
}

size_t xjs_utf16_to_utf8(const uint16_t *src, size_t srclen, char *dst,
                         size_t dstcap) {
  size_t o = 0;
  for (size_t i = 0; i < srclen; ++i) {
    uint32_t cp = src[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < srclen) {
      uint32_t lo = src[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    if (cp < 0x80) {
      if (dst && o + 1 <= dstcap) dst[o] = (char)cp;
      o += 1;
    } else if (cp < 0x800) {
      if (dst && o + 2 <= dstcap) {
        dst[o]     = (char)(0xC0 | (cp >> 6));
        dst[o + 1] = (char)(0x80 | (cp & 0x3F));
      }
      o += 2;
    } else if (cp < 0x10000) {
      if (dst && o + 3 <= dstcap) {
        dst[o]     = (char)(0xE0 | (cp >> 12));
        dst[o + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[o + 2] = (char)(0x80 | (cp & 0x3F));
      }
      o += 3;
    } else {
      if (dst && o + 4 <= dstcap) {
        dst[o]     = (char)(0xF0 | (cp >> 18));
        dst[o + 1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[o + 2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[o + 3] = (char)(0x80 | (cp & 0x3F));
      }
      o += 4;
    }
  }
  return o;
}

JSValue xjs_qv_from_string(JSContext *qctx, xJSStringRef s) {
  if (!s) return JS_NewString(qctx, "");
  /* quickjs-ng stores JS strings as either Latin-1 or UTF-16 code
   * units, so we can hand it our xJSStringRef buffer verbatim via
   * JS_NewStringUTF16().  This skips the UTF-16→UTF-8→UTF-16 round
   * trip the old path did and, importantly, preserves unpaired
   * surrogates byte-for-byte (matches JSC's JSStringCreateWith-
   * Characters contract: a JS string is a sequence of UTF-16 code
   * units, not a validated Unicode scalar sequence). */
  return JS_NewStringUTF16(qctx, s->data, s->length);
}
