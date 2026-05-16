/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_private_test.cpp - White-box tests for the cross-TU helpers
 * declared in js_private.h.  These tests live in the xjs build so
 * they can reach into the private header.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * UTF-8 <-> UTF-16 conversion
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivUtf, AsciiRoundTrip) {
  const char *src = "hello";
  /* sizing pass */
  size_t n16 = xjs_utf8_to_utf16(src, 5, nullptr, 0);
  EXPECT_EQ(n16, 5u);

  uint16_t buf[8] = {};
  size_t   w      = xjs_utf8_to_utf16(src, 5, buf, 8);
  EXPECT_EQ(w, 5u);
  for (size_t i = 0; i < 5; ++i)
    EXPECT_EQ(buf[i], (uint16_t)src[i]);

  /* back to UTF-8 */
  size_t n8 = xjs_utf16_to_utf8(buf, 5, nullptr, 0);
  EXPECT_EQ(n8, 5u);
  char out[8] = {};
  EXPECT_EQ(xjs_utf16_to_utf8(buf, 5, out, 8), 5u);
  EXPECT_EQ(memcmp(out, src, 5), 0);
}

TEST(XjsPrivUtf, TwoByteBmp) {
  /* U+00E9 (é) = 0xC3 0xA9 */
  const char *src  = "\xC3\xA9";
  uint16_t    u[2] = {};
  EXPECT_EQ(xjs_utf8_to_utf16(src, 2, u, 2), 1u);
  EXPECT_EQ(u[0], 0x00E9);
  char out[4] = {};
  EXPECT_EQ(xjs_utf16_to_utf8(u, 1, out, 4), 2u);
  EXPECT_EQ((unsigned char)out[0], 0xC3);
  EXPECT_EQ((unsigned char)out[1], 0xA9);
}

TEST(XjsPrivUtf, ThreeByteBmp) {
  /* U+4E2D (中) = 0xE4 0xB8 0xAD */
  const char *src  = "\xE4\xB8\xAD";
  uint16_t    u[2] = {};
  EXPECT_EQ(xjs_utf8_to_utf16(src, 3, u, 2), 1u);
  EXPECT_EQ(u[0], 0x4E2D);
  char out[4] = {};
  EXPECT_EQ(xjs_utf16_to_utf8(u, 1, out, 4), 3u);
}

TEST(XjsPrivUtf, SurrogatePair) {
  /* U+1F600 (😀) = 0xF0 0x9F 0x98 0x80 */
  const char *src  = "\xF0\x9F\x98\x80";
  size_t      need = xjs_utf8_to_utf16(src, 4, nullptr, 0);
  EXPECT_EQ(need, 2u);
  uint16_t u[2] = {};
  EXPECT_EQ(xjs_utf8_to_utf16(src, 4, u, 2), 2u);
  EXPECT_GE(u[0], 0xD800);
  EXPECT_LT(u[0], 0xDC00);
  EXPECT_GE(u[1], 0xDC00);
  EXPECT_LT(u[1], 0xE000);

  size_t n8 = xjs_utf16_to_utf8(u, 2, nullptr, 0);
  EXPECT_EQ(n8, 4u);
  char out[8] = {};
  EXPECT_EQ(xjs_utf16_to_utf8(u, 2, out, 8), 4u);
  EXPECT_EQ(memcmp(out, src, 4), 0);
}

TEST(XjsPrivUtf, InvalidLeadingByte) {
  /* 0xFF is never a valid UTF-8 byte -> decoded as U+FFFD */
  const char *src = "\xFF";
  uint16_t    u   = 0;
  EXPECT_EQ(xjs_utf8_to_utf16(src, 1, &u, 1), 1u);
  EXPECT_EQ(u, 0xFFFD);
}

TEST(XjsPrivUtf, TruncatedMultiByte) {
  /* 0xE4 starts a 3-byte sequence but we only pass 1 byte */
  const char *src = "\xE4";
  uint16_t    u   = 0;
  EXPECT_EQ(xjs_utf8_to_utf16(src, 1, &u, 1), 1u);
  EXPECT_EQ(u, 0xFFFD);
}

TEST(XjsPrivUtf, InvalidContinuation) {
  /* 0xC3 expects 0x80-0xBF continuation; give 0x20 instead */
  const char *src  = "\xC3\x20";
  uint16_t    u[2] = {};
  EXPECT_EQ(xjs_utf8_to_utf16(src, 2, u, 2), 1u);
  EXPECT_EQ(u[0], 0xFFFD);
}

TEST(XjsPrivUtf, Utf16LoneHighSurrogate) {
  /* Lone high surrogate -> encoder falls through to 3-byte branch */
  uint16_t u = 0xD800;
  size_t   n = xjs_utf16_to_utf8(&u, 1, nullptr, 0);
  EXPECT_EQ(n, 3u);
  char out[4] = {};
  EXPECT_EQ(xjs_utf16_to_utf8(&u, 1, out, 4), 3u);
}

TEST(XjsPrivUtf, Utf16UndersizedDstReportsNeed) {
  /* Providing a dst buffer smaller than required: function should
   * still report the total number of bytes needed. */
  const char *src  = "\xF0\x9F\x98\x80"; /* 😀 */
  uint16_t    u[2] = {};
  xjs_utf8_to_utf16(src, 4, u, 2);
  char   out[2] = {};
  size_t need   = xjs_utf16_to_utf8(u, 2, out, 2);
  EXPECT_EQ(need, 4u); /* 4 bytes needed even though dst only holds 2 */
}

/* ═══════════════════════════════════════════════════════════════════
 * Context helpers: xjs_ctx_of / xjs_ctx_mut
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivCtxHelpers, NullSafe) {
  EXPECT_EQ(xjs_ctx_of(nullptr), nullptr);
  EXPECT_EQ(xjs_ctx_mut(nullptr), nullptr);
}

TEST(XjsPrivCtxHelpers, ReturnsInternalPointers) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  /* xjs_ctx_of should return the same JSContext* that's stashed in
   * the opaque struct.  We don't try to dereference the pointer —
   * that would require the real quickjs.h — but we do verify
   * identity. */
  EXPECT_EQ(xjs_ctx_of(ctx), ctx->ctx);
  EXPECT_EQ(xjs_ctx_mut(ctx), ctx);

  xJSGlobalContextRelease(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Slot refcount plumbing
 *
 * We can't construct a raw JSValue here (that would need quickjs.h),
 * so we validate the reference-count machinery by going through the
 * public API that produces slots, then poking the `refcount` field
 * (which is the first member of OpaqueXJSValue and therefore stable
 * regardless of sizeof(JSValue)).
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivSlot, RetainReleaseRefcount) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  /* Pick a fractional number so the slot is heap-allocated (small
   * integers are inlined as tagged pointers and carry no refcount). */
  xJSValueRef v = xJSValueMakeNumber(ctx, 3.14);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(((uintptr_t)v & XJS_TAG_MASK), XJS_TAG_HEAP);

  struct OpaqueXJSValue *s = (struct OpaqueXJSValue *)v;
  EXPECT_EQ(s->refcount, 1);
  xjs_slot_retain(v);
  EXPECT_EQ(s->refcount, 2);
  xjs_slot_release(v);
  EXPECT_EQ(s->refcount, 1);

  /* Null-safe */
  xjs_slot_retain(nullptr);
  xjs_slot_release(nullptr);

  xjs_slot_release(v); /* frees */

  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivSlot, ExceptionSentinelFailsGracefully) {
  /* xjs_slot_make with JS_EXCEPTION should return NULL — we trigger
   * that indirectly by running JS that throws: the public Eval API
   * uses xjs_slot_make internally and returns NULL on exception. */
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  xJSStringRef s   = xJSStringCreateWithUTF8CString("throw 1");
  xJSValueRef  exc = nullptr;
  xJSValueRef  r   = xJSEvaluateScript(ctx, s, nullptr, nullptr, 0, &exc);
  EXPECT_EQ(r, nullptr);
  /* Exception should have been drained via xjs_propagate_exception. */
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xJSStringRelease(s);
  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivSlot, PropagateExceptionNoPendingIsNoOp) {
  /* When no exception is pending (e.g. after a successful eval),
   * another API call that internally uses xjs_propagate_exception
   * must leave the caller's out-param untouched. */
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  xJSStringRef s   = xJSStringCreateWithUTF8CString("1 + 1");
  xJSValueRef  exc = reinterpret_cast<xJSValueRef>(0xDEADBEEF);
  xJSValueRef  r   = xJSEvaluateScript(ctx, s, nullptr, nullptr, 0, &exc);
  ASSERT_NE(r, nullptr);
  /* Happy path: exception out-param should not be touched. */
  EXPECT_EQ(exc, reinterpret_cast<xJSValueRef>(0xDEADBEEF));
  xjs_slot_release(r);
  xJSStringRelease(s);
  xJSGlobalContextRelease(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * xjs_qv_from_string (exercised indirectly through xJSValueMakeString)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivQVFromString, UnicodeRoundTrip) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  xJSStringRef s   = xJSStringCreateWithUTF8CString("héllo中😀");
  xJSValueRef  jsv = xJSValueMakeString(ctx, s);
  ASSERT_NE(jsv, nullptr);
  xJSStringRef back = xJSValueToStringCopy(ctx, jsv, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(back, "héllo中😀"));
  xJSStringRelease(back);
  xjs_slot_release(jsv);
  xJSStringRelease(s);

  /* NULL input should produce an empty JS string, not a crash. */
  xJSValueRef emp = xJSValueMakeString(ctx, nullptr);
  ASSERT_NE(emp, nullptr);
  xJSStringRef e = xJSValueToStringCopy(ctx, emp, nullptr);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(xJSStringGetLength(e), 0u);
  xJSStringRelease(e);
  xjs_slot_release(emp);

  xJSGlobalContextRelease(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Tagged-pointer encoding (singleton / int31 / heap)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivTagged, SingletonsAreNotHeap) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  xJSValueRef u  = xJSValueMakeUndefined(ctx);
  xJSValueRef n  = xJSValueMakeNull(ctx);
  xJSValueRef tt = xJSValueMakeBoolean(ctx, true);
  xJSValueRef ff = xJSValueMakeBoolean(ctx, false);
  ASSERT_NE(u, nullptr);
  ASSERT_NE(n, nullptr);
  ASSERT_NE(tt, nullptr);
  ASSERT_NE(ff, nullptr);

  /* Each constant collapses to a fixed tagged-pointer bit pattern. */
  EXPECT_EQ((uintptr_t)u, XJS_SINGLETON_UNDEFINED);
  EXPECT_EQ((uintptr_t)n, XJS_SINGLETON_NULL);
  EXPECT_EQ((uintptr_t)tt, XJS_SINGLETON_TRUE);
  EXPECT_EQ((uintptr_t)ff, XJS_SINGLETON_FALSE);

  /* Same call returns an identical tagged pointer every time. */
  EXPECT_EQ(xJSValueMakeUndefined(ctx), u);
  EXPECT_EQ(xJSValueMakeBoolean(ctx, true), tt);

  /* Retain / release on inline values are no-ops (and must not crash). */
  xjs_slot_retain(u);
  xjs_slot_release(u);
  xjs_slot_release(u);
  xjs_slot_release(n);
  xjs_slot_release(tt);
  xjs_slot_release(ff);

  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivTagged, SingletonSemanticsThroughAPI) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  xJSValueRef         u   = xJSValueMakeUndefined(ctx);
  xJSValueRef         n   = xJSValueMakeNull(ctx);
  xJSValueRef         tt  = xJSValueMakeBoolean(ctx, true);
  xJSValueRef         ff  = xJSValueMakeBoolean(ctx, false);

  EXPECT_TRUE(xJSValueIsUndefined(ctx, u));
  EXPECT_TRUE(xJSValueIsNull(ctx, n));
  EXPECT_TRUE(xJSValueIsBoolean(ctx, tt));
  EXPECT_TRUE(xJSValueIsBoolean(ctx, ff));
  EXPECT_TRUE(xJSValueToBoolean(ctx, tt));
  EXPECT_FALSE(xJSValueToBoolean(ctx, ff));
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx, tt, tt));
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx, tt, ff));

  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivTagged, Int31InlineRoundTrip) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  /* In-range ints must be inlined — no heap slot, no refcount. */
  const int32_t samples[] = {
    0, 1, -1, 42, -42, 123456, -123456, XJS_INT31_MIN, XJS_INT31_MAX};
  for (int32_t x : samples) {
    xJSValueRef v = xJSValueMakeNumber(ctx, (double)x);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(((uintptr_t)v & XJS_TAG_MASK), XJS_TAG_INT31) << "value=" << x;
    EXPECT_EQ(xjs_int31_get(v), x);
    EXPECT_TRUE(xJSValueIsNumber(ctx, v));
    EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx, v, nullptr), (double)x);

    /* Retain/release are no-ops on inline values. */
    xjs_slot_retain(v);
    xjs_slot_release(v);
    xjs_slot_release(v);
  }
  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivTagged, Int31OutOfRangeFallsBackToHeap) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  /* XJS_INT31_MAX+1 and XJS_INT31_MIN-1 can't fit in int31 so they
   * should be stored as heap-allocated float64 slots. */
  const int64_t overs[] = {(int64_t)XJS_INT31_MAX + 1,
                           (int64_t)XJS_INT31_MIN - 1, (int64_t)INT32_MAX,
                           (int64_t)INT32_MIN};
  for (int64_t x : overs) {
    xJSValueRef v = xJSValueMakeNumber(ctx, (double)x);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(((uintptr_t)v & XJS_TAG_MASK), XJS_TAG_HEAP) << "value=" << x;
    EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx, v, nullptr), (double)x);
    xjs_slot_release(v);
  }

  /* Non-integer numbers always go to heap. */
  xJSValueRef pi = xJSValueMakeNumber(ctx, 3.14);
  ASSERT_NE(pi, nullptr);
  EXPECT_EQ(((uintptr_t)pi & XJS_TAG_MASK), XJS_TAG_HEAP);
  xjs_slot_release(pi);

  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivTagged, HeapSlotsAreEightByteAligned) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  /* Every heap slot address must have its low 3 bits clear so the
   * tag bits can coexist with the pointer. */
  for (int i = 0; i < 32; ++i) {
    xJSValueRef v = xJSValueMakeNumber(ctx, 0.5 + i);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(((uintptr_t)v & XJS_TAG_MASK), 0u);
    xjs_slot_release(v);
  }
  xJSGlobalContextRelease(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Slot arena: growth + freelist reuse
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsPrivArena, FreelistReusesReleasedCells) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);

  xJSValueRef a  = xJSValueMakeNumber(ctx, 1.5);
  void       *pa = (void *)a;
  xjs_slot_release(a);

  /* The next heap allocation should come back through the freelist,
   * reusing the exact cell we just released. */
  xJSValueRef b = xJSValueMakeNumber(ctx, 2.5);
  EXPECT_EQ((void *)b, pa);
  xjs_slot_release(b);

  xJSGlobalContextRelease(ctx);
}

TEST(XjsPrivArena, GrowsBeyondInitialChunk) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);

  /* Initial chunk capacity is 16.  Force the arena to grow by holding
   * more than that many live heap slots simultaneously. */
  constexpr int kN = 200;
  xJSValueRef   live[kN];
  for (int i = 0; i < kN; ++i) {
    live[i] = xJSValueMakeNumber(ctx, (double)i + 0.5);
    ASSERT_NE(live[i], nullptr);
    ASSERT_EQ(((uintptr_t)live[i] & XJS_TAG_MASK), XJS_TAG_HEAP);
  }
  /* All slots must be distinct addresses. */
  for (int i = 0; i < kN; ++i) {
    for (int j = i + 1; j < kN; ++j) {
      ASSERT_NE(live[i], live[j]);
    }
  }
  for (int i = 0; i < kN; ++i)
    xjs_slot_release(live[i]);

  xJSGlobalContextRelease(ctx);
}
