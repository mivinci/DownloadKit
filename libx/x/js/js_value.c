/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_value.c - xjs value type queries, construction, conversion and
 * Protect/Unprotect refcounting.
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>

#include "quickjs.h"

/* ═══════════════════════════════════════════════════════════════════
 * Type queries
 * ═══════════════════════════════════════════════════════════════════ */

xJSType xJSValueGetType(xJSContextRef c, xJSValueRef v) {
  (void)c;
  if (!v) return kXJSTypeUndefined;
  JSValue qv = xjs_slot_qv(v);
  if (JS_IsUndefined(qv)) return kXJSTypeUndefined;
  if (JS_IsNull(qv)) return kXJSTypeNull;
  if (JS_IsBool(qv)) return kXJSTypeBoolean;
  if (JS_IsNumber(qv)) return kXJSTypeNumber;
  if (JS_IsString(qv)) return kXJSTypeString;
  if (JS_IsSymbol(qv)) return kXJSTypeSymbol;
  return kXJSTypeObject;
}

#define VAL_IS(fn, qcheck)                        \
  bool fn(xJSContextRef c, xJSValueRef v) {       \
    (void)c;                                      \
    if (!v) return false;                         \
    return qcheck(xjs_slot_qv(v)) ? true : false; \
  }
VAL_IS(xJSValueIsUndefined, JS_IsUndefined)
VAL_IS(xJSValueIsNull, JS_IsNull)
VAL_IS(xJSValueIsBoolean, JS_IsBool)
VAL_IS(xJSValueIsNumber, JS_IsNumber)
VAL_IS(xJSValueIsString, JS_IsString)
VAL_IS(xJSValueIsSymbol, JS_IsSymbol)
VAL_IS(xJSValueIsObject, JS_IsObject)
#undef VAL_IS

bool xJSValueIsArray(xJSContextRef c, xJSValueRef v) {
  (void)c;
  if (!v) return false;
  return JS_IsArray(xjs_slot_qv(v));
}

bool xJSValueIsDate(xJSContextRef c, xJSValueRef v) {
  (void)c;
  if (!v) return false;
  /* quickjs-ng exposes JS_IsDate() which inspects the internal class
   * id (JS_CLASS_DATE).  Unlike an `instanceof Date` probe through
   * globalThis.Date, it's immune to user shadowing of the global. */
  return JS_IsDate(xjs_slot_qv(v));
}

bool xJSValueIsObjectOfClass(xJSContextRef c, xJSValueRef v, xJSClassRef k) {
  (void)c;
  if (!v || !k) return false;
  JSValue qv = xjs_slot_qv(v);
  if (!JS_IsObject(qv)) return false;
  /* Every native xjs class is backed by a unique JSClassID allocated
   * in xJSClassCreate().  QuickJS stamps that id onto the JSObject
   * header at creation time (via JS_NewObjectClass) and exposes it
   * through JS_GetClassID.  A plain JS object — or an instance of a
   * different xjs class — returns a different id. */
  return JS_GetClassID(qv) == k->qclass;
}

bool xJSValueIsStrictEqual(xJSContextRef c, xJSValueRef a, xJSValueRef b) {
  if (a == b) return true;
  if (!a || !b) return false;
  return JS_IsStrictEqual(xjs_ctx_of(c), xjs_slot_qv(a), xjs_slot_qv(b))
           ? true
           : false;
}

bool xJSValueIsEqual(xJSContextRef c, xJSValueRef a, xJSValueRef b,
                     xJSValueRef *exception) {
  /* quickjs-ng exposes the full abstract-equality operator (== per
   * ECMA-262 7.2.15), including type coercion.  It returns -1 on
   * exception (e.g. a throwing @@toPrimitive), else 0/1. */
  if (a == b) return true;
  if (!a || !b) return false;
  JSContext *q = xjs_ctx_of(c);
  int        r = JS_IsEqual(q, xjs_slot_qv(a), xjs_slot_qv(b));
  if (r < 0) {
    xjs_propagate_exception(q, exception);
    return false;
  }
  return r == 1;
}

bool xJSValueIsInstanceOfConstructor(xJSContextRef c, xJSValueRef v,
                                     xJSObjectRef ctor, xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!v || !ctor) return false;
  int r = JS_IsInstanceOf(q, xjs_slot_qv(v), xjs_slot_qv((xJSValueRef)ctor));
  if (r < 0) {
    xjs_propagate_exception(q, exc);
    return false;
  }
  return r == 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Construction
 * ═══════════════════════════════════════════════════════════════════ */

xJSValueRef xJSValueMakeUndefined(xJSContextRef c) {
  JSContext *q = xjs_ctx_of(c);
  return xjs_slot_make(q, JS_UNDEFINED);
}

xJSValueRef xJSValueMakeNull(xJSContextRef c) {
  return xjs_slot_make(xjs_ctx_of(c), JS_NULL);
}

xJSValueRef xJSValueMakeBoolean(xJSContextRef c, bool b) {
  JSContext *q = xjs_ctx_of(c);
  return xjs_slot_make(q, JS_NewBool(q, b));
}

xJSValueRef xJSValueMakeNumber(xJSContextRef c, double d) {
  JSContext *q = xjs_ctx_of(c);
  /* Prefer JS_NewInt32 when d is exactly representable as an int32:
   * quickjs-ng's JS_NewFloat64 no longer collapses whole numbers into
   * JS_TAG_INT (bellard did), so we'd otherwise lose the int31 inline
   * encoding for common cases like small counts and indices. */
  if (d >= (double)INT32_MIN && d <= (double)INT32_MAX) {
    int32_t i = (int32_t)d;
    if ((double)i == d) return xjs_slot_make(q, JS_NewInt32(q, i));
  }
  return xjs_slot_make(q, JS_NewFloat64(q, d));
}

xJSValueRef xJSValueMakeString(xJSContextRef c, xJSStringRef s) {
  JSContext *q = xjs_ctx_of(c);
  return xjs_slot_make(q, xjs_qv_from_string(q, s));
}

xJSValueRef xJSValueMakeSymbol(xJSContextRef c, xJSStringRef description) {
  JSContext *q = xjs_ctx_of(c);
  /* quickjs-ng exposes JS_NewSymbol() directly; use it instead of
   * calling the language-level Symbol() constructor through the
   * global.  is_global=false yields a plain unique Symbol (as if by
   * `Symbol(desc)`); the Symbol.for(key) flavor would pass true. */
  JSValue sym;
  if (description) {
    /* JS_NewSymbol takes a UTF-8 C string; xJSStringRef is UTF-16,
     * so round-trip through the documented UTF-8 conversion API. */
    size_t cap = xJSStringGetMaximumUTF8CStringSize(description);
    char  *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    xJSStringGetUTF8CString(description, buf, cap);
    sym = JS_NewSymbol(q, buf, /*is_global=*/false);
    free(buf);
  } else {
    sym = JS_NewSymbol(q, "", /*is_global=*/false);
  }

  if (JS_IsException(sym)) {
    /* Swallow the exception: MakeSymbol has no out-exception param.
     * Returning NULL matches how other xjs Make* helpers fail. */
    JS_FreeValue(q, JS_GetException(q));
    return NULL;
  }
  return xjs_slot_make(q, sym);
}

xJSValueRef xJSValueMakeFromJSONString(xJSContextRef c, xJSStringRef json) {
  JSContext *q = xjs_ctx_of(c);
  if (!json) return NULL;
  size_t nbytes = xjs_utf16_to_utf8(json->data, json->length, NULL, 0);
  char  *buf    = (char *)malloc(nbytes + 1);
  if (!buf) return NULL;
  xjs_utf16_to_utf8(json->data, json->length, buf, nbytes);
  buf[nbytes] = 0;
  JSValue v   = JS_ParseJSON(q, buf, nbytes, "<xjs:json>");
  free(buf);
  return xjs_slot_make(q, v);
}

xJSStringRef xJSValueCreateJSONString(xJSContextRef c, xJSValueRef v,
                                      unsigned indent, xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!v) return NULL;
  JSValue spc = JS_NewInt32(q, (int32_t)indent);
  JSValue j   = JS_JSONStringify(q, xjs_slot_qv(v), JS_UNDEFINED, spc);
  JS_FreeValue(q, spc);
  if (JS_IsException(j)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  size_t       len  = 0;
  const char  *cstr = JS_ToCStringLen(q, &len, j);
  xJSStringRef s    = cstr ? xJSStringCreateWithUTF8CString(cstr) : NULL;
  if (cstr) JS_FreeCString(q, cstr);
  JS_FreeValue(q, j);
  return s;
}

/* ═══════════════════════════════════════════════════════════════════
 * Conversion
 * ═══════════════════════════════════════════════════════════════════ */

bool xJSValueToBoolean(xJSContextRef c, xJSValueRef v) {
  if (!v) return false;
  int r = JS_ToBool(xjs_ctx_of(c), xjs_slot_qv(v));
  return r == 1;
}

double xJSValueToNumber(xJSContextRef c, xJSValueRef v, xJSValueRef *exc) {
  if (!v) return 0;
  double d = 0;
  if (JS_ToFloat64(xjs_ctx_of(c), &d, xjs_slot_qv(v)) < 0) {
    xjs_propagate_exception(xjs_ctx_of(c), exc);
    return 0;
  }
  return d;
}

xJSStringRef xJSValueToStringCopy(xJSContextRef c, xJSValueRef v,
                                  xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!v) return NULL;
  JSValue s = JS_ToString(q, xjs_slot_qv(v));
  if (JS_IsException(s)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  /* JS strings are UTF-16 code-unit sequences per spec; quickjs-ng
   * exposes the raw code units via JS_ToCStringLenUTF16(), skipping a
   * UTF-16→UTF-8→UTF-16 round-trip and preserving unpaired surrogates
   * the same way JSC's JSValueToStringCopy does. */
  size_t          ulen = 0;
  const uint16_t *u16  = JS_ToCStringLenUTF16(q, &ulen, s);
  xJSStringRef    out  = u16 ? xJSStringCreateWithCharacters(u16, ulen) : NULL;
  if (u16) JS_FreeCStringUTF16(q, u16);
  JS_FreeValue(q, s);
  return out;
}

xJSObjectRef xJSValueToObject(xJSContextRef c, xJSValueRef v,
                              xJSValueRef *exc) {
  JSContext *q = xjs_ctx_of(c);
  if (!v) return NULL;
  JSValue qv = xjs_slot_qv(v);

  /* Already an object — just bump the slot's refcount and re-tag.
   * Skipping JS_ToObject here avoids an unnecessary js_dup/JS_FreeValue
   * pair on the hot path and keeps the returned xJSObjectRef aliased
   * to the same slot (matches the JSC-shaped identity contract). */
  if (JS_IsObject(qv)) {
    xjs_slot_retain(v);
    return (xJSObjectRef)v;
  }

  /* Primitive → wrapper object.  JS_ToObject() implements the ECMA
   * ToObject abstract operation directly against internal class ids
   * (Number/String/Boolean/Symbol/BigInt), and throws the exact
   * TypeError we want for undefined/null — no need to go through
   * globalThis.Object (which a malicious script could shadow). */
  JSValue boxed = JS_ToObject(q, qv);
  if (JS_IsException(boxed)) {
    xjs_propagate_exception(q, exc);
    return NULL;
  }
  xJSValueRef slot = xjs_slot_make(q, boxed);
  return (xJSObjectRef)slot;
}

void xJSValueProtect(xJSContextRef c, xJSValueRef v) {
  (void)c;
  xjs_slot_retain(v);
}
void xJSValueUnprotect(xJSContextRef c, xJSValueRef v) {
  (void)c;
  xjs_slot_release(v);
}
