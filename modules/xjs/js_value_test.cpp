/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_value_test.cpp - xJSValue type queries, construction, conversion,
 * JSON bridge, Protect/Unprotect.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

/* ─────────── shared fixture ─────────── */

namespace {
class XjsValueTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = xJSGlobalContextCreate(nullptr);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) xJSGlobalContextRelease(ctx_);
  }
  xJSValueRef eval(const char *src, xJSValueRef *exc = nullptr) {
    xJSStringRef s = xJSStringCreateWithUTF8CString(src);
    xJSValueRef  v = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 0, exc);
    xJSStringRelease(s);
    return v;
  }
  xJSGlobalContextRef ctx_ = nullptr;
};
} // namespace

/* ─────────── Construction / type queries ─────────── */

TEST_F(XjsValueTest, Undefined) {
  xJSValueRef v = xJSValueMakeUndefined(ctx_);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(xJSValueGetType(ctx_, v), kXJSTypeUndefined);
  EXPECT_TRUE(xJSValueIsUndefined(ctx_, v));
  EXPECT_FALSE(xJSValueIsNull(ctx_, v));
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, Null) {
  xJSValueRef v = xJSValueMakeNull(ctx_);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(xJSValueGetType(ctx_, v), kXJSTypeNull);
  EXPECT_TRUE(xJSValueIsNull(ctx_, v));
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, Boolean) {
  xJSValueRef t = xJSValueMakeBoolean(ctx_, true);
  xJSValueRef f = xJSValueMakeBoolean(ctx_, false);
  ASSERT_NE(t, nullptr);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(xJSValueGetType(ctx_, t), kXJSTypeBoolean);
  EXPECT_TRUE(xJSValueIsBoolean(ctx_, t));
  EXPECT_TRUE(xJSValueToBoolean(ctx_, t));
  EXPECT_FALSE(xJSValueToBoolean(ctx_, f));
  xjs_slot_release(t);
  xjs_slot_release(f);
}

TEST_F(XjsValueTest, Number) {
  xJSValueRef v = xJSValueMakeNumber(ctx_, 3.14);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(xJSValueGetType(ctx_, v), kXJSTypeNumber);
  EXPECT_TRUE(xJSValueIsNumber(ctx_, v));
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 3.14);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, StringRoundTrip) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("hi");
  xJSValueRef  v = xJSValueMakeString(ctx_, s);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(xJSValueGetType(ctx_, v), kXJSTypeString);
  EXPECT_TRUE(xJSValueIsString(ctx_, v));

  xJSStringRef back = xJSValueToStringCopy(ctx_, v, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(back, "hi"));

  xJSStringRelease(s);
  xJSStringRelease(back);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, StringFromNullRef) {
  xJSValueRef v = xJSValueMakeString(ctx_, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(xJSValueIsString(ctx_, v));
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, NullRefAllQueriesFalse) {
  EXPECT_EQ(xJSValueGetType(ctx_, nullptr), kXJSTypeUndefined);
  EXPECT_FALSE(xJSValueIsUndefined(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsNull(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsBoolean(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsNumber(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsString(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsSymbol(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsObject(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsArray(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsDate(ctx_, nullptr));
  EXPECT_FALSE(xJSValueIsObjectOfClass(ctx_, nullptr, nullptr));
  EXPECT_FALSE(xJSValueToBoolean(ctx_, nullptr));
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, nullptr, nullptr), 0.0);
  EXPECT_EQ(xJSValueToStringCopy(ctx_, nullptr, nullptr), nullptr);
  EXPECT_EQ(xJSValueToObject(ctx_, nullptr, nullptr), nullptr);
}

/* ─────────── Symbol ─────────── */

TEST_F(XjsValueTest, MakeSymbolWithDescription) {
  xJSStringRef desc = xJSStringCreateWithUTF8CString("marker");
  xJSValueRef  s    = xJSValueMakeSymbol(ctx_, desc);
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(xJSValueIsSymbol(ctx_, s));
  EXPECT_EQ(xJSValueGetType(ctx_, s), kXJSTypeSymbol);

  /* ECMA ToString(Symbol) throws TypeError, so we can't use
   * xJSValueToStringCopy directly.  Instead read sym.description via
   * JS, which returns the plain string passed to the constructor. */
  xJSStringRef getDescSrc = xJSStringCreateWithUTF8CString(
    "(function(sym){ return sym.description; })");
  xJSValueRef fn =
    xJSEvaluateScript(ctx_, getDescSrc, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(fn, nullptr);
  xJSValueRef arg = s;
  xJSValueRef out =
    xJSObjectCallAsFunction(ctx_, (xJSObjectRef)fn, nullptr, 1, &arg, nullptr);
  ASSERT_NE(out, nullptr);
  xJSStringRef back = xJSValueToStringCopy(ctx_, out, nullptr);
  ASSERT_NE(back, nullptr);
  char buf[32] = {0};
  xJSStringGetUTF8CString(back, buf, sizeof(buf));
  EXPECT_STREQ(buf, "marker");

  xJSStringRelease(back);
  xjs_slot_release(out);
  xjs_slot_release(fn);
  xJSStringRelease(getDescSrc);
  xJSStringRelease(desc);
  xjs_slot_release(s);
}

TEST_F(XjsValueTest, MakeSymbolWithoutDescription) {
  xJSValueRef s = xJSValueMakeSymbol(ctx_, nullptr);
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(xJSValueIsSymbol(ctx_, s));
  xjs_slot_release(s);
}

TEST_F(XjsValueTest, MakeSymbolProducesUniqueIdentity) {
  xJSStringRef d  = xJSStringCreateWithUTF8CString("x");
  xJSValueRef  a  = xJSValueMakeSymbol(ctx_, d);
  xJSValueRef  b  = xJSValueMakeSymbol(ctx_, d);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  /* Symbol() always mints a fresh, un-interned symbol — even with the
   * same description.  Verifies we went through the real constructor
   * and not some caching shortcut. */
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, a, b));
  xJSStringRelease(d);
  xjs_slot_release(a);
  xjs_slot_release(b);
}

TEST_F(XjsValueTest, IsSymbolDetectsRealSymbol) {
  /* Use JS to produce a real Symbol value. */
  xJSValueRef s = eval("Symbol('x')");
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(xJSValueIsSymbol(ctx_, s));
  EXPECT_EQ(xJSValueGetType(ctx_, s), kXJSTypeSymbol);
  xjs_slot_release(s);
}

/* ─────────── Array / Date / ObjectOfClass ─────────── */

TEST_F(XjsValueTest, IsArrayPositive) {
  xJSValueRef a = eval("[1,2,3]");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(xJSValueIsArray(ctx_, a));
  EXPECT_TRUE(xJSValueIsObject(ctx_, a));
  xjs_slot_release(a);
}

TEST_F(XjsValueTest, IsArrayNegative) {
  xJSValueRef o = eval("({})");
  ASSERT_NE(o, nullptr);
  EXPECT_FALSE(xJSValueIsArray(ctx_, o));
  xjs_slot_release(o);
}

TEST_F(XjsValueTest, IsDatePositive) {
  xJSValueRef d = eval("new Date()");
  ASSERT_NE(d, nullptr);
  EXPECT_TRUE(xJSValueIsDate(ctx_, d));
  xjs_slot_release(d);
}

TEST_F(XjsValueTest, IsDateNegatives) {
  /* plain object */
  xJSValueRef o = eval("({})");
  EXPECT_FALSE(xJSValueIsDate(ctx_, o));
  xjs_slot_release(o);

  /* primitive */
  xJSValueRef n = xJSValueMakeNumber(ctx_, 1);
  EXPECT_FALSE(xJSValueIsDate(ctx_, n));
  xjs_slot_release(n);

  /* array */
  xJSValueRef a = eval("[]");
  EXPECT_FALSE(xJSValueIsDate(ctx_, a));
  xjs_slot_release(a);
}

TEST_F(XjsValueTest, IsDateSurvivesUserOverrideOfGlobalDate) {
  /* If a user replaces globalThis.Date with something non-callable,
   * IsDate must not crash and must simply report false. */
  xJSValueRef noop = eval("globalThis.Date = 42; null");
  if (noop) xjs_slot_release(noop);

  /* We can't make a new Date now (Date was clobbered), but we can
   * make sure a plain object still reports false cleanly. */
  xJSValueRef o = eval("({})");
  EXPECT_FALSE(xJSValueIsDate(ctx_, o));
  xjs_slot_release(o);
}

TEST_F(XjsValueTest, IsObjectOfClassMatchesOwnClass) {
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = "MyCls";
  xJSClassRef cls        = xJSClassCreate(&def);
  ASSERT_NE(cls, nullptr);

  int dummy = 0;
  xJSObjectRef obj = xJSObjectMake(ctx_, cls, &dummy);
  ASSERT_NE(obj, nullptr);
  EXPECT_TRUE(xJSValueIsObjectOfClass(ctx_, (xJSValueRef)obj, cls));

  xjs_slot_release((xJSValueRef)obj);
  xJSClassRelease(cls);
}

TEST_F(XjsValueTest, IsObjectOfClassRejectsOtherClass) {
  xJSClassDefinition defA = kXJSClassDefinitionEmpty;
  defA.className          = "A";
  xJSClassRef A           = xJSClassCreate(&defA);
  xJSClassDefinition defB = kXJSClassDefinitionEmpty;
  defB.className          = "B";
  xJSClassRef B           = xJSClassCreate(&defB);

  int d = 0;
  xJSObjectRef a = xJSObjectMake(ctx_, A, &d);
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(xJSValueIsObjectOfClass(ctx_, (xJSValueRef)a, A));
  EXPECT_FALSE(xJSValueIsObjectOfClass(ctx_, (xJSValueRef)a, B));

  /* plain JS object belongs to neither */
  xJSValueRef plain = eval("({})");
  EXPECT_FALSE(xJSValueIsObjectOfClass(ctx_, plain, A));
  xjs_slot_release(plain);

  /* null class */
  EXPECT_FALSE(xJSValueIsObjectOfClass(ctx_, (xJSValueRef)a, nullptr));

  xjs_slot_release((xJSValueRef)a);
  xJSClassRelease(A);
  xJSClassRelease(B);
}

TEST_F(XjsValueTest, IsObjectOfClassRejectsPrimitive) {
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = "C";
  xJSClassRef cls        = xJSClassCreate(&def);
  xJSValueRef n          = xJSValueMakeNumber(ctx_, 1);
  EXPECT_FALSE(xJSValueIsObjectOfClass(ctx_, n, cls));
  xjs_slot_release(n);
  xJSClassRelease(cls);
}

/* ─────────── Equality ─────────── */

TEST_F(XjsValueTest, StrictEqSameIdentity) {
  xJSValueRef n = xJSValueMakeNumber(ctx_, 1);
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx_, n, n));
  xjs_slot_release(n);
}

TEST_F(XjsValueTest, StrictEqSameValue) {
  xJSValueRef a = xJSValueMakeNumber(ctx_, 2);
  xJSValueRef b = xJSValueMakeNumber(ctx_, 2);
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx_, a, b));
  xjs_slot_release(a);
  xjs_slot_release(b);
}

TEST_F(XjsValueTest, StrictEqDifferentValue) {
  xJSValueRef a = xJSValueMakeNumber(ctx_, 2);
  xJSValueRef b = xJSValueMakeNumber(ctx_, 3);
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, a, b));
  xjs_slot_release(a);
  xjs_slot_release(b);
}

TEST_F(XjsValueTest, StrictEqNullHandling) {
  xJSValueRef a = xJSValueMakeNumber(ctx_, 1);
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, a, nullptr));
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, nullptr, a));
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx_, nullptr, nullptr));
  xjs_slot_release(a);
}

/* xJSValueIsEqual must implement ECMA `==` (abstract equality with
 * type coercion), not `===`.  These cases are the ones JSC's
 * JSValueIsEqual covers and that a strict-equal fallback would
 * silently get wrong. */
TEST_F(XjsValueTest, AbstractEqualSameNumber) {
  xJSValueRef a = xJSValueMakeNumber(ctx_, 1);
  xJSValueRef b = xJSValueMakeNumber(ctx_, 1);
  EXPECT_TRUE(xJSValueIsEqual(ctx_, a, b, nullptr));
  xjs_slot_release(a);
  xjs_slot_release(b);
}

TEST_F(XjsValueTest, AbstractEqualCoercesNumberAndString) {
  xJSValueRef n = xJSValueMakeNumber(ctx_, 0);
  xJSValueRef s = eval("\"0\"");
  ASSERT_TRUE(s);
  /* 0 === "0" is false, but 0 == "0" must be true. */
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, n, s));
  EXPECT_TRUE(xJSValueIsEqual(ctx_, n, s, nullptr));
  xjs_slot_release(n);
  xjs_slot_release(s);
}

TEST_F(XjsValueTest, AbstractEqualNullAndUndefined) {
  xJSValueRef u = xJSValueMakeUndefined(ctx_);
  xJSValueRef n = xJSValueMakeNull(ctx_);
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, u, n));
  EXPECT_TRUE(xJSValueIsEqual(ctx_, u, n, nullptr));
  xjs_slot_release(u);
  xjs_slot_release(n);
}

TEST_F(XjsValueTest, AbstractEqualBoolCoercesToNumber) {
  xJSValueRef t   = xJSValueMakeBoolean(ctx_, true);
  xJSValueRef one = xJSValueMakeNumber(ctx_, 1);
  EXPECT_FALSE(xJSValueIsStrictEqual(ctx_, t, one));
  EXPECT_TRUE(xJSValueIsEqual(ctx_, t, one, nullptr));
  xjs_slot_release(t);
  xjs_slot_release(one);
}

TEST_F(XjsValueTest, AbstractEqualThrowingToPrimitivePropagates) {
  /* Object whose @@toPrimitive throws — JS_IsEqual must surface that
   * through the exception out-parameter, not silently swallow it. */
  xJSValueRef o = eval(
    "({ [Symbol.toPrimitive]() { throw new Error('boom'); } })");
  ASSERT_TRUE(o);
  xJSValueRef n   = xJSValueMakeNumber(ctx_, 1);
  xJSValueRef exc = nullptr;
  EXPECT_FALSE(xJSValueIsEqual(ctx_, o, n, &exc));
  EXPECT_TRUE(exc != nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release(n);
  xjs_slot_release(o);
}

TEST_F(XjsValueTest, InstanceOfConstructor) {
  xJSValueRef  arr = eval("[1,2,3]");
  xJSObjectRef Arr = (xJSObjectRef)eval("Array");
  ASSERT_NE(arr, nullptr);
  ASSERT_NE(Arr, nullptr);
  EXPECT_TRUE(xJSValueIsInstanceOfConstructor(ctx_, arr, Arr, nullptr));

  xJSValueRef n = xJSValueMakeNumber(ctx_, 1);
  EXPECT_FALSE(xJSValueIsInstanceOfConstructor(ctx_, n, Arr, nullptr));

  /* null-safety */
  EXPECT_FALSE(xJSValueIsInstanceOfConstructor(ctx_, nullptr, Arr, nullptr));
  EXPECT_FALSE(xJSValueIsInstanceOfConstructor(ctx_, arr, nullptr, nullptr));

  xjs_slot_release(arr);
  xjs_slot_release((xJSValueRef)Arr);
  xjs_slot_release(n);
}

TEST_F(XjsValueTest, InstanceOfPropagatesException) {
  /* instanceof against a non-callable raises TypeError. */
  xJSValueRef  v   = xJSValueMakeNumber(ctx_, 1);
  xJSObjectRef bad = (xJSObjectRef)xJSValueMakeNumber(ctx_, 42);
  xJSValueRef  exc = nullptr;
  EXPECT_FALSE(xJSValueIsInstanceOfConstructor(ctx_, v, bad, &exc));
  /* An exception should have been produced. */
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release(v);
  xjs_slot_release((xJSValueRef)bad);
}

/* ─────────── JSON bridge ─────────── */

TEST_F(XjsValueTest, JsonParse) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("{\"a\":1}");
  xJSValueRef  v = xJSValueMakeFromJSONString(ctx_, s);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx_, v));
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, JsonParseNullString) {
  EXPECT_EQ(xJSValueMakeFromJSONString(ctx_, nullptr), nullptr);
}

TEST_F(XjsValueTest, JsonStringify) {
  xJSValueRef v = eval("({a:1,b:[2]})");
  ASSERT_NE(v, nullptr);
  xJSStringRef s = xJSValueCreateJSONString(ctx_, v, 0, nullptr);
  ASSERT_NE(s, nullptr);
  char buf[64] = {0};
  xJSStringGetUTF8CString(s, buf, sizeof(buf));
  EXPECT_STREQ(buf, "{\"a\":1,\"b\":[2]}");
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, JsonStringifyIndented) {
  xJSValueRef  v = eval("({a:1})");
  xJSStringRef s = xJSValueCreateJSONString(ctx_, v, 2, nullptr);
  ASSERT_NE(s, nullptr);
  char buf[64] = {0};
  xJSStringGetUTF8CString(s, buf, sizeof(buf));
  /* Whatever the exact formatting, indent>0 should contain a newline. */
  EXPECT_NE(strchr(buf, '\n'), nullptr);
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, JsonStringifyNullValue) {
  EXPECT_EQ(xJSValueCreateJSONString(ctx_, nullptr, 0, nullptr), nullptr);
}

TEST_F(XjsValueTest, JsonStringifyCircularThrows) {
  /* Circular reference -> TypeError.  Must go through the exception
   * out-param. */
  xJSValueRef  o   = eval("var x={}; x.self=x; x");
  xJSValueRef  exc = nullptr;
  xJSStringRef s   = xJSValueCreateJSONString(ctx_, o, 0, &exc);
  EXPECT_EQ(s, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release(o);
}

/* ─────────── Conversion ─────────── */

TEST_F(XjsValueTest, ToNumberFromString) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("42.5");
  xJSValueRef  v = xJSValueMakeString(ctx_, s);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 42.5);
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, ToNumberFromInvalidIsNaN) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("xyz");
  xJSValueRef  v = xJSValueMakeString(ctx_, s);
  double       d = xJSValueToNumber(ctx_, v, nullptr);
  EXPECT_TRUE(std::isnan(d));
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, ToStringCopyFromNumber) {
  xJSValueRef  v = xJSValueMakeNumber(ctx_, 7);
  xJSStringRef s = xJSValueToStringCopy(ctx_, v, nullptr);
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(s, "7"));
  xJSStringRelease(s);
  xjs_slot_release(v);
}

TEST_F(XjsValueTest, ToObjectFromObject) {
  xJSValueRef o = eval("({x:1})");
  ASSERT_NE(o, nullptr);
  xJSObjectRef obj = xJSValueToObject(ctx_, o, nullptr);
  ASSERT_NE(obj, nullptr);
  /* Already an object — ToObject returns the same slot with an
   * extra retain, no wrapper allocation. */
  EXPECT_EQ((xJSValueRef)obj, o);

  xjs_slot_release((xJSValueRef)obj); /* drop the retain added by ToObject */
  xjs_slot_release(o);
}

TEST_F(XjsValueTest, ToObjectBoxesNumber) {
  xJSValueRef  n   = xJSValueMakeNumber(ctx_, 7);
  xJSObjectRef obj = xJSValueToObject(ctx_, n, nullptr);
  ASSERT_NE(obj, nullptr);
  /* Fresh wrapper object, not the primitive slot. */
  EXPECT_NE((xJSValueRef)obj, n);
  EXPECT_TRUE(xJSValueIsObject(ctx_, (xJSValueRef)obj));
  /* It unboxes back to 7 via valueOf. */
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, (xJSValueRef)obj, nullptr), 7.0);

  xjs_slot_release((xJSValueRef)obj);
  xjs_slot_release(n);
}

TEST_F(XjsValueTest, ToObjectBoxesString) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("hey");
  xJSValueRef  v   = xJSValueMakeString(ctx_, s);
  xJSObjectRef obj = xJSValueToObject(ctx_, v, nullptr);
  ASSERT_NE(obj, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx_, (xJSValueRef)obj));

  /* Round-trip back to string to prove the box wraps the original. */
  xJSStringRef back =
    xJSValueToStringCopy(ctx_, (xJSValueRef)obj, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(back, "hey"));

  xJSStringRelease(s);
  xJSStringRelease(back);
  xjs_slot_release(v);
  xjs_slot_release((xJSValueRef)obj);
}

TEST_F(XjsValueTest, ToObjectBoxesBoolean) {
  xJSValueRef  b   = xJSValueMakeBoolean(ctx_, true);
  xJSObjectRef obj = xJSValueToObject(ctx_, b, nullptr);
  ASSERT_NE(obj, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx_, (xJSValueRef)obj));
  EXPECT_TRUE(xJSValueToBoolean(ctx_, (xJSValueRef)obj));
  xjs_slot_release((xJSValueRef)obj);
  xjs_slot_release(b);
}

TEST_F(XjsValueTest, ToObjectFromUndefinedThrowsTypeError) {
  xJSValueRef u   = xJSValueMakeUndefined(ctx_);
  xJSValueRef exc = nullptr;
  EXPECT_EQ(xJSValueToObject(ctx_, u, &exc), nullptr);
  ASSERT_NE(exc, nullptr);

  xJSStringRef s = xJSValueToStringCopy(ctx_, exc, nullptr);
  ASSERT_NE(s, nullptr);
  char buf[64] = {0};
  xJSStringGetUTF8CString(s, buf, sizeof(buf));
  /* QuickJS wording contains "TypeError". */
  EXPECT_TRUE(strstr(buf, "TypeError") != nullptr ||
              strstr(buf, "convert")   != nullptr);
  xJSStringRelease(s);
  xjs_slot_release(exc);
  xjs_slot_release(u);
}

TEST_F(XjsValueTest, ToObjectFromNullThrowsTypeError) {
  xJSValueRef n   = xJSValueMakeNull(ctx_);
  xJSValueRef exc = nullptr;
  EXPECT_EQ(xJSValueToObject(ctx_, n, &exc), nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release(n);
}

/* ─────────── Protect / Unprotect ─────────── */

TEST_F(XjsValueTest, ProtectUnprotectAdjustRefcount) {
  /* Use a fractional number so the slot is heap-allocated; tagged
   * inline values have no refcount to adjust. */
  xJSValueRef v = xJSValueMakeNumber(ctx_, 3.14);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(((uintptr_t)v & XJS_TAG_MASK), XJS_TAG_HEAP);
  EXPECT_EQ(v->refcount, 1);
  xJSValueProtect(ctx_, v);
  EXPECT_EQ(v->refcount, 2);
  xJSValueUnprotect(ctx_, v);
  EXPECT_EQ(v->refcount, 1);
  xjs_slot_release(v);
}
