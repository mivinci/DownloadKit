/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_object_test.cpp - Object construction, property access,
 * call/construct, and property-name array/accumulator.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>

namespace {
class XjsObjectTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = xJSGlobalContextCreate(nullptr);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) xJSGlobalContextRelease(ctx_);
  }
  xJSValueRef eval(const char *src) {
    xJSStringRef s = xJSStringCreateWithUTF8CString(src);
    xJSValueRef  v = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 0, nullptr);
    xJSStringRelease(s);
    return v;
  }
  xJSObjectRef evalObj(const char *src) {
    return (xJSObjectRef)eval(src);
  }
  xJSGlobalContextRef ctx_ = nullptr;
};
} // namespace

/* ─────────── kXJSClassDefinitionEmpty ─────────── */

TEST(XjsClassDefEmpty, AllZero) {
  const xJSClassDefinition &d = kXJSClassDefinitionEmpty;
  EXPECT_EQ(d.version, 0);
  EXPECT_EQ(d.attributes, 0u);
  EXPECT_EQ(d.className, nullptr);
  EXPECT_EQ(d.parentClass, nullptr);
  EXPECT_EQ(d.staticValues, nullptr);
  EXPECT_EQ(d.staticFunctions, nullptr);
  EXPECT_EQ(d.initialize, nullptr);
  EXPECT_EQ(d.finalize, nullptr);
  EXPECT_EQ(d.callAsFunction, nullptr);
}

/* ─────────── Make ─────────── */

TEST_F(XjsObjectTest, MakePlainObject) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  ASSERT_NE(o, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx_, (xJSValueRef)o));
  EXPECT_FALSE(xJSValueIsArray(ctx_, (xJSValueRef)o));
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, MakeArrayEmpty) {
  xJSObjectRef a = xJSObjectMakeArray(ctx_, 0, nullptr, nullptr);
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(xJSValueIsArray(ctx_, (xJSValueRef)a));
  xjs_slot_release((xJSValueRef)a);
}

TEST_F(XjsObjectTest, MakeArrayWithItems) {
  xJSValueRef  a      = xJSValueMakeNumber(ctx_, 1);
  xJSValueRef  b      = xJSValueMakeNumber(ctx_, 2);
  xJSValueRef  args[] = {a, b};
  xJSObjectRef arr    = xJSObjectMakeArray(ctx_, 2, args, nullptr);
  ASSERT_NE(arr, nullptr);
  EXPECT_TRUE(xJSValueIsArray(ctx_, (xJSValueRef)arr));

  /* length should be 2 */
  xJSStringRef kLen = xJSStringCreateWithUTF8CString("length");
  xJSValueRef  len  = xJSObjectGetProperty(ctx_, arr, kLen, nullptr);
  ASSERT_NE(len, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, len, nullptr), 2.0);
  xJSStringRelease(kLen);
  xjs_slot_release(len);

  /* [0] should be 1 */
  xJSValueRef e0 = xJSObjectGetPropertyAtIndex(ctx_, arr, 0, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, e0, nullptr), 1.0);
  xjs_slot_release(e0);

  xjs_slot_release(a);
  xjs_slot_release(b);
  xjs_slot_release((xJSValueRef)arr);
}

TEST_F(XjsObjectTest, MakeErrorWithMessage) {
  xJSStringRef m      = xJSStringCreateWithUTF8CString("boom");
  xJSValueRef  msg    = xJSValueMakeString(ctx_, m);
  xJSValueRef  args[] = {msg};
  xJSObjectRef e      = xJSObjectMakeError(ctx_, 1, args, nullptr);
  ASSERT_NE(e, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx_, (xJSValueRef)e));

  xJSStringRef kMsg = xJSStringCreateWithUTF8CString("message");
  xJSValueRef  mv   = xJSObjectGetProperty(ctx_, e, kMsg, nullptr);
  ASSERT_NE(mv, nullptr);
  xJSStringRef got = xJSValueToStringCopy(ctx_, mv, nullptr);
  ASSERT_NE(got, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(got, "boom"));
  xJSStringRelease(got);
  xjs_slot_release(mv);
  xJSStringRelease(kMsg);

  xJSStringRelease(m);
  xjs_slot_release(msg);
  xjs_slot_release((xJSValueRef)e);
}

TEST_F(XjsObjectTest, MakeErrorNoArgs) {
  xJSObjectRef e = xJSObjectMakeError(ctx_, 0, nullptr, nullptr);
  ASSERT_NE(e, nullptr);
  xjs_slot_release((xJSValueRef)e);
}

/* ─────────── Stubs we want to lock down ─────────── */

/* All Make* routines are real implementations now; null-callback
 * safety is covered in their dedicated test suites below.          */

TEST_F(XjsObjectTest, GetSetPrivateAlwaysFail) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  EXPECT_EQ(xJSObjectGetPrivate(o), nullptr);
  int dummy = 0;
  EXPECT_FALSE(xJSObjectSetPrivate(o, &dummy));
  /* Null-safe */
  EXPECT_EQ(xJSObjectGetPrivate(nullptr), nullptr);
  EXPECT_FALSE(xJSObjectSetPrivate(nullptr, nullptr));
  xjs_slot_release((xJSValueRef)o);
}

/* ─────────── Prototype ─────────── */

TEST_F(XjsObjectTest, GetSetPrototype) {
  xJSObjectRef child  = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSObjectRef parent = xJSObjectMake(ctx_, nullptr, nullptr);

  xJSObjectSetPrototype(ctx_, child, (xJSValueRef)parent);
  xJSValueRef got = xJSObjectGetPrototype(ctx_, child);
  ASSERT_NE(got, nullptr);
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx_, got, (xJSValueRef)parent));

  /* NULL-safe: must not crash */
  EXPECT_EQ(xJSObjectGetPrototype(ctx_, nullptr), nullptr);
  xJSObjectSetPrototype(ctx_, nullptr, (xJSValueRef)parent);
  xJSObjectSetPrototype(ctx_, child, nullptr);

  xjs_slot_release(got);
  xjs_slot_release((xJSValueRef)child);
  xjs_slot_release((xJSValueRef)parent);
}

/* ─────────── Property get/set/has/delete by name ─────────── */

TEST_F(XjsObjectTest, PropertyByName) {
  xJSObjectRef o   = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k   = xJSStringCreateWithUTF8CString("foo");
  xJSValueRef  val = xJSValueMakeNumber(ctx_, 7);

  EXPECT_FALSE(xJSObjectHasProperty(ctx_, o, k));

  xJSObjectSetProperty(ctx_, o, k, val, kXJSPropertyAttributeNone, nullptr);
  EXPECT_TRUE(xJSObjectHasProperty(ctx_, o, k));

  xJSValueRef got = xJSObjectGetProperty(ctx_, o, k, nullptr);
  ASSERT_NE(got, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, got, nullptr), 7.0);
  xjs_slot_release(got);

  EXPECT_TRUE(xJSObjectDeleteProperty(ctx_, o, k, nullptr));
  EXPECT_FALSE(xJSObjectHasProperty(ctx_, o, k));

  xJSStringRelease(k);
  xjs_slot_release(val);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, PropertyByNameNullSafe) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k = xJSStringCreateWithUTF8CString("x");

  EXPECT_FALSE(xJSObjectHasProperty(ctx_, nullptr, k));
  EXPECT_FALSE(xJSObjectHasProperty(ctx_, o, nullptr));
  EXPECT_EQ(xJSObjectGetProperty(ctx_, nullptr, k, nullptr), nullptr);
  EXPECT_EQ(xJSObjectGetProperty(ctx_, o, nullptr, nullptr), nullptr);
  xJSObjectSetProperty(ctx_, nullptr, k, nullptr, 0, nullptr); /* no crash */
  xJSObjectSetProperty(ctx_, o, nullptr, nullptr, 0, nullptr);
  EXPECT_FALSE(xJSObjectDeleteProperty(ctx_, nullptr, k, nullptr));
  EXPECT_FALSE(xJSObjectDeleteProperty(ctx_, o, nullptr, nullptr));

  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, SetPropertyValueNullStoresUndefined) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k = xJSStringCreateWithUTF8CString("u");
  xJSObjectSetProperty(ctx_, o, k, nullptr, 0, nullptr);
  xJSValueRef v = xJSObjectGetProperty(ctx_, o, k, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(xJSValueIsUndefined(ctx_, v));
  xjs_slot_release(v);
  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

/* ─────────── Property attributes (ReadOnly / DontEnum / DontDelete) ─────────── */

TEST_F(XjsObjectTest, SetPropertyReadOnlyRejectsAssignment) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k = xJSStringCreateWithUTF8CString("ro");

  xJSValueRef seven = xJSValueMakeNumber(ctx_, 7);
  xJSObjectSetProperty(ctx_, o, k, seven, kXJSPropertyAttributeReadOnly,
                       nullptr);
  xjs_slot_release(seven);

  /* Subsequent assignment from JS silently fails in sloppy mode;
   * the value must stay 7. */
  xJSStringRef js = xJSStringCreateWithUTF8CString(
    "(function(o){ o.ro = 99; return o.ro; })");
  xJSValueRef  fn  = xJSEvaluateScript(ctx_, js, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(fn, nullptr);
  xJSValueRef  arg = (xJSValueRef)o;
  xJSValueRef  ret = xJSObjectCallAsFunction(ctx_, (xJSObjectRef)fn, nullptr,
                                             1, &arg, nullptr);
  ASSERT_NE(ret, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, ret, nullptr), 7.0);

  xjs_slot_release(ret);
  xjs_slot_release(fn);
  xJSStringRelease(js);
  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, SetPropertyDontEnumHidesFromKeys) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef kHidden  = xJSStringCreateWithUTF8CString("hidden");
  xJSStringRef kVisible = xJSStringCreateWithUTF8CString("visible");
  xJSValueRef  one      = xJSValueMakeNumber(ctx_, 1);
  xJSValueRef  two      = xJSValueMakeNumber(ctx_, 2);

  xJSObjectSetProperty(ctx_, o, kHidden, one, kXJSPropertyAttributeDontEnum,
                       nullptr);
  xJSObjectSetProperty(ctx_, o, kVisible, two, kXJSPropertyAttributeNone,
                       nullptr);

  /* Object.keys() skips non-enumerable own props. */
  xJSStringRef js = xJSStringCreateWithUTF8CString(
    "(function(o){ return Object.keys(o).join(','); })");
  xJSValueRef fn = xJSEvaluateScript(ctx_, js, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(fn, nullptr);
  xJSValueRef arg = (xJSValueRef)o;
  xJSValueRef ret = xJSObjectCallAsFunction(ctx_, (xJSObjectRef)fn, nullptr,
                                            1, &arg, nullptr);
  xJSStringRef s = xJSValueToStringCopy(ctx_, ret, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(s, "visible"));

  /* But the hidden property is still readable by name. */
  xJSValueRef direct = xJSObjectGetProperty(ctx_, o, kHidden, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, direct, nullptr), 1.0);

  xjs_slot_release(direct);
  xJSStringRelease(s);
  xjs_slot_release(ret);
  xjs_slot_release(fn);
  xJSStringRelease(js);
  xjs_slot_release(one);
  xjs_slot_release(two);
  xJSStringRelease(kHidden);
  xJSStringRelease(kVisible);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, SetPropertyDontDeleteBlocksDeletion) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k = xJSStringCreateWithUTF8CString("locked");
  xJSValueRef  v = xJSValueMakeNumber(ctx_, 42);
  xJSObjectSetProperty(ctx_, o, k, v, kXJSPropertyAttributeDontDelete,
                       nullptr);
  xjs_slot_release(v);

  /* `delete` on a non-configurable property returns false in sloppy
   * mode; the property stays. */
  bool deleted = xJSObjectDeleteProperty(ctx_, o, k, nullptr);
  EXPECT_FALSE(deleted);
  EXPECT_TRUE(xJSObjectHasProperty(ctx_, o, k));

  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, SetPropertyCombinedAttributes) {
  /* All three flags at once: effectively freeze the slot. */
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef k = xJSStringCreateWithUTF8CString("frozen");
  xJSValueRef  v = xJSValueMakeNumber(ctx_, 5);
  xJSObjectSetProperty(ctx_, o, k, v,
                       kXJSPropertyAttributeReadOnly |
                         kXJSPropertyAttributeDontEnum |
                         kXJSPropertyAttributeDontDelete,
                       nullptr);
  xjs_slot_release(v);

  /* Readonly: assignment via JS no-ops in sloppy mode. */
  xJSStringRef js = xJSStringCreateWithUTF8CString(
    "(function(o){ o.frozen = 9; return o.frozen; })");
  xJSValueRef fn = xJSEvaluateScript(ctx_, js, nullptr, nullptr, 0, nullptr);
  xJSValueRef arg = (xJSValueRef)o;
  xJSValueRef ret = xJSObjectCallAsFunction(ctx_, (xJSObjectRef)fn, nullptr,
                                            1, &arg, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, ret, nullptr), 5.0);
  xjs_slot_release(ret);
  xjs_slot_release(fn);
  xJSStringRelease(js);

  /* DontDelete: delete fails. */
  EXPECT_FALSE(xJSObjectDeleteProperty(ctx_, o, k, nullptr));

  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, SetPropertyNoFlagsPreservesSetterSemantics) {
  /* When no attribute flags are passed, SetProperty goes through
   * [[Set]] so prototype setters fire — unlike the DefineProperty
   * path used for flag-bearing calls. */
  xJSObjectRef o = evalObj(
    "({ _v: 0, "
    "   set trip(x) { this._v = x * 10; }, "
    "   get trip()  { return this._v; } "
    "})");
  ASSERT_NE(o, nullptr);
  xJSStringRef k     = xJSStringCreateWithUTF8CString("trip");
  xJSValueRef  three = xJSValueMakeNumber(ctx_, 3);
  xJSObjectSetProperty(ctx_, o, k, three, kXJSPropertyAttributeNone, nullptr);
  xjs_slot_release(three);

  xJSValueRef got = xJSObjectGetProperty(ctx_, o, k, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, got, nullptr), 30.0);
  xjs_slot_release(got);

  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, GetPropertyPropagatesException) {
  /* Proxy trap throwing — access raises. */
  xJSObjectRef o =
    evalObj("new Proxy({}, { get() { throw new Error('bad'); } })");
  ASSERT_NE(o, nullptr);
  xJSStringRef k   = xJSStringCreateWithUTF8CString("anything");
  xJSValueRef  exc = nullptr;
  xJSValueRef  v   = xJSObjectGetProperty(ctx_, o, k, &exc);
  EXPECT_EQ(v, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xJSStringRelease(k);
  xjs_slot_release((xJSValueRef)o);
}

/* ─────────── Property by index ─────────── */

TEST_F(XjsObjectTest, PropertyByIndex) {
  xJSObjectRef a = xJSObjectMakeArray(ctx_, 0, nullptr, nullptr);
  xJSValueRef  v = xJSValueMakeNumber(ctx_, 99);
  xJSObjectSetPropertyAtIndex(ctx_, a, 0, v, nullptr);
  xJSObjectSetPropertyAtIndex(ctx_, a, 1, nullptr, nullptr); /* undefined */

  xJSValueRef g0 = xJSObjectGetPropertyAtIndex(ctx_, a, 0, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, g0, nullptr), 99.0);
  xjs_slot_release(g0);

  xJSValueRef g1 = xJSObjectGetPropertyAtIndex(ctx_, a, 1, nullptr);
  EXPECT_TRUE(xJSValueIsUndefined(ctx_, g1));
  xjs_slot_release(g1);

  /* null-safe */
  EXPECT_EQ(xJSObjectGetPropertyAtIndex(ctx_, nullptr, 0, nullptr), nullptr);
  xJSObjectSetPropertyAtIndex(ctx_, nullptr, 0, v, nullptr);

  xjs_slot_release(v);
  xjs_slot_release((xJSValueRef)a);
}

/* ─────────── Function / constructor ─────────── */

TEST_F(XjsObjectTest, IsFunctionAndCall) {
  /* function add(a,b){return a+b;} */
  xJSObjectRef fn = evalObj("(function(a,b){return a+b;})");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(xJSObjectIsFunction(ctx_, fn));
  EXPECT_FALSE(xJSObjectIsFunction(ctx_, nullptr));

  xJSValueRef a      = xJSValueMakeNumber(ctx_, 3);
  xJSValueRef b      = xJSValueMakeNumber(ctx_, 4);
  xJSValueRef args[] = {a, b};
  xJSValueRef r = xJSObjectCallAsFunction(ctx_, fn, nullptr, 2, args, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r, nullptr), 7.0);
  xjs_slot_release(r);

  /* Call with no args */
  xJSObjectRef zero = evalObj("(function(){return 42})");
  xJSValueRef  r2 =
    xJSObjectCallAsFunction(ctx_, zero, nullptr, 0, nullptr, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r2, nullptr), 42.0);
  xjs_slot_release(r2);
  xjs_slot_release((xJSValueRef)zero);

  xjs_slot_release(a);
  xjs_slot_release(b);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, CallThrowsPropagates) {
  xJSObjectRef fn  = evalObj("(function(){throw new Error('x')})");
  xJSValueRef  exc = nullptr;
  xJSValueRef  r = xJSObjectCallAsFunction(ctx_, fn, nullptr, 0, nullptr, &exc);
  EXPECT_EQ(r, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, CallNullObjectReturnsNull) {
  EXPECT_EQ(
    xJSObjectCallAsFunction(ctx_, nullptr, nullptr, 0, nullptr, nullptr),
    nullptr);
}

TEST_F(XjsObjectTest, IsConstructorAndNew) {
  xJSObjectRef ctor = evalObj("(function Point(x){this.x=x;})");
  ASSERT_NE(ctor, nullptr);
  EXPECT_TRUE(xJSObjectIsConstructor(ctx_, ctor));
  EXPECT_FALSE(xJSObjectIsConstructor(ctx_, nullptr));

  xJSValueRef  x      = xJSValueMakeNumber(ctx_, 10);
  xJSValueRef  args[] = {x};
  xJSObjectRef inst = xJSObjectCallAsConstructor(ctx_, ctor, 1, args, nullptr);
  ASSERT_NE(inst, nullptr);
  xJSStringRef kX = xJSStringCreateWithUTF8CString("x");
  xJSValueRef  v  = xJSObjectGetProperty(ctx_, inst, kX, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 10.0);
  xjs_slot_release(v);
  xJSStringRelease(kX);

  xjs_slot_release(x);
  xjs_slot_release((xJSValueRef)inst);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, ConstructorThrowsPropagates) {
  xJSObjectRef ctor = evalObj("(function(){throw new Error('c')})");
  xJSValueRef  exc  = nullptr;
  xJSObjectRef r    = xJSObjectCallAsConstructor(ctx_, ctor, 0, nullptr, &exc);
  EXPECT_EQ(r, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, ConstructNullObjectReturnsNull) {
  EXPECT_EQ(xJSObjectCallAsConstructor(ctx_, nullptr, 0, nullptr, nullptr),
            nullptr);
}

/* ─────────── PropertyNameArray (manual construction) ─────────── */

TEST(XjsPropNameArray, RetainReleaseAndAccess) {
  /* Exercises the pure refcount / accessor paths without going
   * through xJSObjectCopyPropertyNames — handy when debugging to
   * isolate storage bugs from enumeration bugs. */
  struct OpaqueXJSPropertyNameArray *a =
    (struct OpaqueXJSPropertyNameArray *)calloc(
      1, sizeof(struct OpaqueXJSPropertyNameArray));
  ASSERT_NE(a, nullptr);
  a->refcount = 1;
  a->count    = 2;
  a->names    = (xJSStringRef *)calloc(2, sizeof(xJSStringRef));
  a->names[0] = xJSStringCreateWithUTF8CString("foo");
  a->names[1] = xJSStringCreateWithUTF8CString("bar");

  xJSPropertyNameArrayRef ref = (xJSPropertyNameArrayRef)a;
  EXPECT_EQ(xJSPropertyNameArrayGetCount(ref), 2u);
  xJSStringRef n0 = xJSPropertyNameArrayGetNameAtIndex(ref, 0);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(n0, "foo"));
  xJSStringRef n1 = xJSPropertyNameArrayGetNameAtIndex(ref, 1);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(n1, "bar"));
  EXPECT_EQ(xJSPropertyNameArrayGetNameAtIndex(ref, 5), nullptr);

  xJSPropertyNameArrayRef r2 = xJSPropertyNameArrayRetain(ref);
  EXPECT_EQ(r2, ref);
  EXPECT_EQ(a->refcount, 2);

  xJSPropertyNameArrayRelease(ref);
  EXPECT_EQ(a->refcount, 1);
  xJSPropertyNameArrayRelease(ref); /* frees */
}

TEST(XjsPropNameArray, NullSafe) {
  EXPECT_EQ(xJSPropertyNameArrayRetain(nullptr), nullptr);
  xJSPropertyNameArrayRelease(nullptr);
  EXPECT_EQ(xJSPropertyNameArrayGetCount(nullptr), 0u);
  EXPECT_EQ(xJSPropertyNameArrayGetNameAtIndex(nullptr, 0), nullptr);
}

/* ─────────── xJSObjectCopyPropertyNames (real API) ─────────── */

TEST_F(XjsObjectTest, CopyPropertyNamesListsOwnEnumerableStringKeys) {
  /* Mix: three enumerable string keys in insertion order — a, b, c. */
  xJSObjectRef o = evalObj("({ a: 1, b: 2, c: 3 })");
  ASSERT_NE(o, nullptr);

  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  ASSERT_EQ(xJSPropertyNameArrayGetCount(names), 3u);

  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 0), "a"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 1), "b"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 2), "c"));

  xJSPropertyNameArrayRelease(names);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesEmpty) {
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  ASSERT_NE(o, nullptr);
  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  EXPECT_EQ(xJSPropertyNameArrayGetCount(names), 0u);
  EXPECT_EQ(xJSPropertyNameArrayGetNameAtIndex(names, 0), nullptr);
  xJSPropertyNameArrayRelease(names);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesSkipsNonEnumerable) {
  /* Use DontEnum flag from xJSObjectSetProperty so the output must
   * contain only "visible". */
  xJSObjectRef o = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef kHidden  = xJSStringCreateWithUTF8CString("hidden");
  xJSStringRef kVisible = xJSStringCreateWithUTF8CString("visible");
  xJSValueRef  one      = xJSValueMakeNumber(ctx_, 1);
  xJSValueRef  two      = xJSValueMakeNumber(ctx_, 2);
  xJSObjectSetProperty(ctx_, o, kHidden, one, kXJSPropertyAttributeDontEnum,
                       nullptr);
  xJSObjectSetProperty(ctx_, o, kVisible, two, kXJSPropertyAttributeNone,
                       nullptr);

  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  ASSERT_EQ(xJSPropertyNameArrayGetCount(names), 1u);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 0), "visible"));

  xJSPropertyNameArrayRelease(names);
  xjs_slot_release(one);
  xjs_slot_release(two);
  xJSStringRelease(kHidden);
  xJSStringRelease(kVisible);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesSkipsSymbolKeys) {
  /* Object with a Symbol key should not leak that key out through
   * the string-key enumeration surface. */
  xJSObjectRef o = evalObj(
    "(() => { const s = Symbol('x'); const o = { plain: 1 }; "
    "  o[s] = 2; return o; })()");
  ASSERT_NE(o, nullptr);
  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  EXPECT_EQ(xJSPropertyNameArrayGetCount(names), 1u);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 0), "plain"));
  xJSPropertyNameArrayRelease(names);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesOwnOnlyNotInherited) {
  /* Prototype chain keys must not appear. */
  xJSObjectRef o = evalObj(
    "(() => { class Parent { p() {} }; class Child extends Parent { "
    "   constructor() { super(); this.own = 1; } }; return new Child(); })()");
  ASSERT_NE(o, nullptr);
  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  EXPECT_EQ(xJSPropertyNameArrayGetCount(names), 1u);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 0), "own"));
  xJSPropertyNameArrayRelease(names);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesPreservesInsertionOrderWithIndices) {
  /* Numeric-index keys get canonicalised to array-index atoms by
   * QuickJS and come out first (per JS spec), then string keys in
   * insertion order. */
  xJSObjectRef o = evalObj("({ 2: 'x', 0: 'y', foo: 1, 1: 'z', bar: 2 })");
  ASSERT_NE(o, nullptr);
  xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(names, nullptr);
  ASSERT_EQ(xJSPropertyNameArrayGetCount(names), 5u);
  /* Indices ascending first. */
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 0), "0"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 1), "1"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 2), "2"));
  /* Then string keys in insertion order. */
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 3), "foo"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(
    xJSPropertyNameArrayGetNameAtIndex(names, 4), "bar"));
  xJSPropertyNameArrayRelease(names);
  xjs_slot_release((xJSValueRef)o);
}

TEST_F(XjsObjectTest, CopyPropertyNamesNullObjectReturnsNull) {
  EXPECT_EQ(xJSObjectCopyPropertyNames(ctx_, nullptr), nullptr);
}

TEST_F(XjsObjectTest, CopyPropertyNamesRetainReleaseLifecycle) {
  /* End-to-end retain/release on a real array. */
  xJSObjectRef o = evalObj("({ k: 1 })");
  ASSERT_NE(o, nullptr);
  xJSPropertyNameArrayRef a = xJSObjectCopyPropertyNames(ctx_, o);
  ASSERT_NE(a, nullptr);
  xJSPropertyNameArrayRef a2 = xJSPropertyNameArrayRetain(a);
  EXPECT_EQ(a, a2);
  xJSPropertyNameArrayRelease(a);  /* refcount 2 → 1 */
  /* Still readable after one release: */
  EXPECT_EQ(xJSPropertyNameArrayGetCount(a2), 1u);
  xJSPropertyNameArrayRelease(a2); /* frees */
  xjs_slot_release((xJSValueRef)o);
}

/* ─────────── PropertyNameAccumulator ─────────── */

TEST(XjsPropNameAccumulator, AddGrows) {
  struct OpaqueXJSPropertyNameAccumulator acc = {nullptr, 0, 0};
  xJSPropertyNameAccumulatorRef           ref = &acc;

  for (int i = 0; i < 20; ++i) {
    char name[16];
    snprintf(name, sizeof(name), "k%d", i);
    xJSStringRef s = xJSStringCreateWithUTF8CString(name);
    xJSPropertyNameAccumulatorAddName(ref, s);
    xJSStringRelease(s); /* accumulator took its own retain */
  }
  EXPECT_EQ(acc.count, 20u);
  EXPECT_GE(acc.capacity, 20u);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(acc.names[0], "k0"));
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(acc.names[19], "k19"));

  /* Null-safe */
  xJSPropertyNameAccumulatorAddName(nullptr, nullptr);
  xJSPropertyNameAccumulatorAddName(ref, nullptr);
  EXPECT_EQ(acc.count, 20u);

  /* Cleanup */
  for (size_t i = 0; i < acc.count; ++i)
    xJSStringRelease(acc.names[i]);
  free(acc.names);
}

/* ═══════════════════════════════════════════════════════════════════
 * Host-function objects (xJSObjectMakeFunctionWithCallback)
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

/* Side-channel counter so tests can verify the trampoline actually
 * reached the user callback (and how many times). */
std::atomic<int> g_host_fn_calls{0};

xJSValueRef host_fn_return_42(xJSContextRef ctx, xJSObjectRef /*function*/,
                              xJSObjectRef /*thisObject*/, size_t /*argc*/,
                              const xJSValueRef /*argv*/[],
                              xJSValueRef * /*exception*/) {
  g_host_fn_calls.fetch_add(1);
  return xJSValueMakeNumber(ctx, 42);
}

xJSValueRef host_fn_sum(xJSContextRef ctx, xJSObjectRef /*function*/,
                        xJSObjectRef /*thisObject*/, size_t argc,
                        const xJSValueRef argv[], xJSValueRef *exception) {
  double s = 0;
  for (size_t i = 0; i < argc; ++i)
    s += xJSValueToNumber(ctx, argv[i], exception);
  return xJSValueMakeNumber(ctx, s);
}

xJSValueRef host_fn_throw(xJSContextRef ctx, xJSObjectRef /*function*/,
                          xJSObjectRef /*thisObject*/, size_t /*argc*/,
                          const xJSValueRef /*argv*/[],
                          xJSValueRef *exception) {
  if (exception) *exception = xJSValueMakeNumber(ctx, 99);
  return nullptr;
}

xJSValueRef host_fn_identity_this(xJSContextRef /*ctx*/,
                                  xJSObjectRef /*function*/,
                                  xJSObjectRef thisObject, size_t /*argc*/,
                                  const xJSValueRef /*argv*/[],
                                  xJSValueRef * /*exception*/) {
  return (xJSValueRef)thisObject;
}

}  // namespace

TEST_F(XjsObjectTest, MakeFunctionReturnsCallableObject) {
  g_host_fn_calls = 0;
  xJSStringRef name = xJSStringCreateWithUTF8CString("return42");
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, name,
                                                     host_fn_return_42);
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(xJSObjectIsFunction(ctx_, fn));
  EXPECT_FALSE(xJSValueIsUndefined(ctx_, (xJSValueRef)fn));

  xJSValueRef r = xJSObjectCallAsFunction(ctx_, fn, nullptr, 0, nullptr,
                                          nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, r, nullptr), 42.0);
  EXPECT_EQ(g_host_fn_calls.load(), 1);

  xjs_slot_release(r);
  xjs_slot_release((xJSValueRef)fn);
  xJSStringRelease(name);
}

TEST_F(XjsObjectTest, MakeFunctionExposesNameProperty) {
  xJSStringRef name = xJSStringCreateWithUTF8CString("myFunc");
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, name,
                                                     host_fn_return_42);
  ASSERT_NE(fn, nullptr);

  xJSStringRef nameKey = xJSStringCreateWithUTF8CString("name");
  xJSValueRef  got     = xJSObjectGetProperty(ctx_, fn, nameKey, nullptr);
  ASSERT_NE(got, nullptr);
  EXPECT_TRUE(xJSValueIsString(ctx_, got));
  xJSStringRef gotStr = xJSValueToStringCopy(ctx_, got, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(gotStr, "myFunc"));
  xJSStringRelease(gotStr);

  xjs_slot_release(got);
  xJSStringRelease(nameKey);
  xJSStringRelease(name);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, MakeFunctionForwardsArguments) {
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, nullptr,
                                                     host_fn_sum);
  ASSERT_NE(fn, nullptr);

  xJSValueRef a = xJSValueMakeNumber(ctx_, 10);
  xJSValueRef b = xJSValueMakeNumber(ctx_, 20);
  xJSValueRef c = xJSValueMakeNumber(ctx_, 5);
  xJSValueRef args[] = {a, b, c};
  xJSValueRef r = xJSObjectCallAsFunction(ctx_, fn, nullptr, 3, args, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, r, nullptr), 35.0);

  xjs_slot_release(r);
  xjs_slot_release(c);
  xjs_slot_release(b);
  xjs_slot_release(a);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, MakeFunctionPropagatesException) {
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, nullptr,
                                                     host_fn_throw);
  ASSERT_NE(fn, nullptr);

  xJSValueRef exc = nullptr;
  xJSValueRef r = xJSObjectCallAsFunction(ctx_, fn, nullptr, 0, nullptr, &exc);
  EXPECT_EQ(r, nullptr);
  ASSERT_NE(exc, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, exc, nullptr), 99.0);

  xjs_slot_release(exc);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, MakeFunctionForwardsThis) {
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, nullptr,
                                                     host_fn_identity_this);
  ASSERT_NE(fn, nullptr);
  xJSObjectRef thisObj = xJSObjectMake(ctx_, nullptr, nullptr);
  ASSERT_NE(thisObj, nullptr);

  xJSValueRef r = xJSObjectCallAsFunction(ctx_, fn, thisObj, 0, nullptr,
                                          nullptr);
  ASSERT_NE(r, nullptr);
  /* The callback returned thisObject directly; QuickJS should have
   * received the same underlying object and round-tripped it back. */
  EXPECT_TRUE(xJSValueIsStrictEqual(ctx_, r, (xJSValueRef)thisObj));

  xjs_slot_release(r);
  xjs_slot_release((xJSValueRef)thisObj);
  xjs_slot_release((xJSValueRef)fn);
}

TEST_F(XjsObjectTest, MakeFunctionCallableFromScript) {
  /* Wire our C callback into JS and invoke it from source code. */
  g_host_fn_calls = 0;
  xJSStringRef name = xJSStringCreateWithUTF8CString("nativeReturn42");
  xJSObjectRef fn = xJSObjectMakeFunctionWithCallback(ctx_, name,
                                                     host_fn_return_42);
  ASSERT_NE(fn, nullptr);

  xJSObjectRef g = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, name, (xJSValueRef)fn,
                       kXJSPropertyAttributeNone, nullptr);

  xJSStringRef src = xJSStringCreateWithUTF8CString("nativeReturn42()");
  xJSValueRef  r   = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, r, nullptr), 42.0);
  EXPECT_EQ(g_host_fn_calls.load(), 1);

  xjs_slot_release(r);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)g);
  xjs_slot_release((xJSValueRef)fn);
  xJSStringRelease(name);
}

TEST_F(XjsObjectTest, MakeFunctionRejectsNullCallback) {
  xJSStringRef name = xJSStringCreateWithUTF8CString("x");
  EXPECT_EQ(xJSObjectMakeFunctionWithCallback(ctx_, name, nullptr), nullptr);
  EXPECT_EQ(xJSObjectMakeFunctionWithCallback(ctx_, nullptr, nullptr),
            nullptr);
  xJSStringRelease(name);
}

/* ═══════════════════════════════════════════════════════════════════
 * Host constructors (xJSObjectMakeConstructor)
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

std::atomic<int> g_host_ctor_calls{0};

xJSObjectRef host_ctor_make_point(xJSContextRef ctx,
                                  xJSObjectRef /*constructor*/, size_t argc,
                                  const xJSValueRef argv[],
                                  xJSValueRef *exception) {
  g_host_ctor_calls.fetch_add(1);
  xJSObjectRef o = xJSObjectMake(ctx, nullptr, nullptr);
  if (!o) return nullptr;

  double x = argc > 0 ? xJSValueToNumber(ctx, argv[0], exception) : 0.0;
  double y = argc > 1 ? xJSValueToNumber(ctx, argv[1], exception) : 0.0;

  xJSStringRef kX = xJSStringCreateWithUTF8CString("x");
  xJSStringRef kY = xJSStringCreateWithUTF8CString("y");
  xJSValueRef  vx = xJSValueMakeNumber(ctx, x);
  xJSValueRef  vy = xJSValueMakeNumber(ctx, y);
  xJSObjectSetProperty(ctx, o, kX, vx, kXJSPropertyAttributeNone, nullptr);
  xJSObjectSetProperty(ctx, o, kY, vy, kXJSPropertyAttributeNone, nullptr);
  xjs_slot_release(vx);
  xjs_slot_release(vy);
  xJSStringRelease(kX);
  xJSStringRelease(kY);
  return o;
}

xJSObjectRef host_ctor_throw(xJSContextRef ctx,
                             xJSObjectRef /*constructor*/, size_t /*argc*/,
                             const xJSValueRef /*argv*/[],
                             xJSValueRef *exception) {
  if (exception) *exception = xJSValueMakeNumber(ctx, 77);
  return nullptr;
}

xJSObjectRef host_ctor_returns_null(xJSContextRef /*ctx*/,
                                    xJSObjectRef /*constructor*/,
                                    size_t /*argc*/,
                                    const xJSValueRef /*argv*/[],
                                    xJSValueRef * /*exception*/) {
  return nullptr;
}

}  // namespace

TEST_F(XjsObjectTest, MakeConstructorReturnsCallableCtor) {
  g_host_ctor_calls = 0;
  xJSObjectRef ctor =
    xJSObjectMakeConstructor(ctx_, nullptr, host_ctor_make_point);
  ASSERT_NE(ctor, nullptr);
  EXPECT_TRUE(xJSObjectIsConstructor(ctx_, ctor));

  xJSValueRef a = xJSValueMakeNumber(ctx_, 3);
  xJSValueRef b = xJSValueMakeNumber(ctx_, 4);
  xJSValueRef args[] = {a, b};
  xJSObjectRef inst =
    xJSObjectCallAsConstructor(ctx_, ctor, 2, args, nullptr);
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ(g_host_ctor_calls.load(), 1);

  xJSStringRef kX = xJSStringCreateWithUTF8CString("x");
  xJSStringRef kY = xJSStringCreateWithUTF8CString("y");
  xJSValueRef  vx = xJSObjectGetProperty(ctx_, inst, kX, nullptr);
  xJSValueRef  vy = xJSObjectGetProperty(ctx_, inst, kY, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, vx, nullptr), 3.0);
  EXPECT_EQ(xJSValueToNumber(ctx_, vy, nullptr), 4.0);

  xjs_slot_release(vx);
  xjs_slot_release(vy);
  xJSStringRelease(kX);
  xJSStringRelease(kY);
  xjs_slot_release((xJSValueRef)inst);
  xjs_slot_release(a);
  xjs_slot_release(b);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, MakeConstructorPropagatesException) {
  xJSObjectRef ctor =
    xJSObjectMakeConstructor(ctx_, nullptr, host_ctor_throw);
  ASSERT_NE(ctor, nullptr);

  xJSValueRef exc = nullptr;
  xJSObjectRef r =
    xJSObjectCallAsConstructor(ctx_, ctor, 0, nullptr, &exc);
  EXPECT_EQ(r, nullptr);
  ASSERT_NE(exc, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, exc, nullptr), 77.0);

  xjs_slot_release(exc);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, MakeConstructorNullReturnBecomesTypeError) {
  xJSObjectRef ctor =
    xJSObjectMakeConstructor(ctx_, nullptr, host_ctor_returns_null);
  ASSERT_NE(ctor, nullptr);

  xJSValueRef exc = nullptr;
  xJSObjectRef r =
    xJSObjectCallAsConstructor(ctx_, ctor, 0, nullptr, &exc);
  EXPECT_EQ(r, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, MakeConstructorCallableFromScriptWithNew) {
  /* Expose the native constructor to JS and invoke it with `new`. */
  g_host_ctor_calls = 0;
  xJSObjectRef ctor =
    xJSObjectMakeConstructor(ctx_, nullptr, host_ctor_make_point);
  ASSERT_NE(ctor, nullptr);

  xJSStringRef name = xJSStringCreateWithUTF8CString("Point");
  xJSObjectRef g    = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, name, (xJSValueRef)ctor,
                       kXJSPropertyAttributeNone, nullptr);

  xJSStringRef src =
    xJSStringCreateWithUTF8CString("(new Point(7,8)).x + (new Point(7,8)).y");
  xJSValueRef r = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, r, nullptr), 15.0);
  EXPECT_EQ(g_host_ctor_calls.load(), 2);

  xjs_slot_release(r);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)g);
  xJSStringRelease(name);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, MakeConstructorRetainsJsClass) {
  /* When a JSClass is provided, it should be retained for the
   * lifetime of the constructor object (released in its finalizer).
   * We merely smoke-test that providing a class doesn't break
   * construction; deeper class/prototype wiring is future work. */
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = "Point";
  xJSClassRef k          = xJSClassCreate(&def);
  ASSERT_NE(k, nullptr);

  xJSObjectRef ctor =
    xJSObjectMakeConstructor(ctx_, k, host_ctor_make_point);
  ASSERT_NE(ctor, nullptr);
  EXPECT_TRUE(xJSObjectIsConstructor(ctx_, ctor));

  /* Release our own ref; the ctor's retained ref keeps it alive. */
  xJSClassRelease(k);

  xJSObjectRef inst =
    xJSObjectCallAsConstructor(ctx_, ctor, 0, nullptr, nullptr);
  ASSERT_NE(inst, nullptr);

  xjs_slot_release((xJSValueRef)inst);
  xjs_slot_release((xJSValueRef)ctor);
}

TEST_F(XjsObjectTest, MakeConstructorRejectsNullCallback) {
  EXPECT_EQ(xJSObjectMakeConstructor(ctx_, nullptr, nullptr), nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * MakeDate
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(XjsObjectTest, MakeDateNoArgsIsDateInstance) {
  xJSObjectRef d = xJSObjectMakeDate(ctx_, 0, nullptr, nullptr);
  ASSERT_NE(d, nullptr);
  /* `d instanceof Date` — easiest way: expose to JS and eval. */
  xJSStringRef name = xJSStringCreateWithUTF8CString("__d");
  xJSObjectRef g    = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, name, (xJSValueRef)d,
                       kXJSPropertyAttributeNone, nullptr);

  xJSStringRef src = xJSStringCreateWithUTF8CString("__d instanceof Date");
  xJSValueRef r = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(xJSValueToBoolean(ctx_, r));

  xjs_slot_release(r);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)g);
  xJSStringRelease(name);
  xjs_slot_release((xJSValueRef)d);
}

TEST_F(XjsObjectTest, MakeDateFromEpochMs) {
  /* new Date(1700000000000) → getTime() returns the same ms. */
  xJSValueRef  ms      = xJSValueMakeNumber(ctx_, 1700000000000.0);
  xJSValueRef  args[]  = {ms};
  xJSObjectRef d       = xJSObjectMakeDate(ctx_, 1, args, nullptr);
  ASSERT_NE(d, nullptr);

  /* Call getTime() on the instance. */
  xJSStringRef kGet = xJSStringCreateWithUTF8CString("getTime");
  xJSValueRef  fn   = xJSObjectGetProperty(ctx_, d, kGet, nullptr);
  ASSERT_NE(fn, nullptr);
  ASSERT_TRUE(xJSValueIsObject(ctx_, fn));
  xJSObjectRef fobj = xJSValueToObject(ctx_, fn, nullptr);
  xJSValueRef  rv =
    xJSObjectCallAsFunction(ctx_, fobj, d, 0, nullptr, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, rv, nullptr), 1700000000000.0);

  xjs_slot_release(rv);
  xjs_slot_release((xJSValueRef)fobj);
  xjs_slot_release(fn);
  xJSStringRelease(kGet);
  xjs_slot_release(ms);
  xjs_slot_release((xJSValueRef)d);
}

/* ═══════════════════════════════════════════════════════════════════
 * MakeRegExp
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(XjsObjectTest, MakeRegExpNoArgsIsEmptyRegExp) {
  xJSObjectRef r = xJSObjectMakeRegExp(ctx_, 0, nullptr, nullptr);
  ASSERT_NE(r, nullptr);

  /* Expose to JS and verify `instanceof RegExp` + `.source`. */
  xJSStringRef name = xJSStringCreateWithUTF8CString("__r");
  xJSObjectRef g    = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, name, (xJSValueRef)r,
                       kXJSPropertyAttributeNone, nullptr);
  xJSStringRef src = xJSStringCreateWithUTF8CString("__r instanceof RegExp");
  xJSValueRef v = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(xJSValueToBoolean(ctx_, v));

  xjs_slot_release(v);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)g);
  xJSStringRelease(name);
  xjs_slot_release((xJSValueRef)r);
}

TEST_F(XjsObjectTest, MakeRegExpWithPatternAndFlagsTestsMatch) {
  xJSStringRef sp     = xJSStringCreateWithUTF8CString("^foo");
  xJSStringRef sf     = xJSStringCreateWithUTF8CString("i");
  xJSValueRef  vp     = xJSValueMakeString(ctx_, sp);
  xJSValueRef  vf     = xJSValueMakeString(ctx_, sf);
  xJSValueRef  args[] = {vp, vf};
  xJSObjectRef r = xJSObjectMakeRegExp(ctx_, 2, args, nullptr);
  ASSERT_NE(r, nullptr);

  /* r.test("FOObar") === true */
  xJSStringRef kTest = xJSStringCreateWithUTF8CString("test");
  xJSValueRef  fn    = xJSObjectGetProperty(ctx_, r, kTest, nullptr);
  ASSERT_TRUE(xJSValueIsObject(ctx_, fn));
  xJSObjectRef fobj = xJSValueToObject(ctx_, fn, nullptr);

  xJSStringRef ss    = xJSStringCreateWithUTF8CString("FOObar");
  xJSValueRef  inp   = xJSValueMakeString(ctx_, ss);
  xJSValueRef  call_args[] = {inp};
  xJSValueRef  rv = xJSObjectCallAsFunction(ctx_, fobj, r, 1, call_args, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_TRUE(xJSValueToBoolean(ctx_, rv));

  xjs_slot_release(rv);
  xjs_slot_release(inp);
  xJSStringRelease(ss);
  xjs_slot_release((xJSValueRef)fobj);
  xjs_slot_release(fn);
  xJSStringRelease(kTest);
  xjs_slot_release(vp);
  xjs_slot_release(vf);
  xJSStringRelease(sp);
  xJSStringRelease(sf);
  xjs_slot_release((xJSValueRef)r);
}

TEST_F(XjsObjectTest, MakeRegExpInvalidPatternRaisesException) {
  xJSStringRef sp     = xJSStringCreateWithUTF8CString("(");  /* unbalanced */
  xJSValueRef  vp     = xJSValueMakeString(ctx_, sp);
  xJSValueRef  args[] = {vp};
  xJSValueRef  exc    = nullptr;
  xJSObjectRef r = xJSObjectMakeRegExp(ctx_, 1, args, &exc);
  EXPECT_EQ(r, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xjs_slot_release(vp);
  xJSStringRelease(sp);
}

/* ═══════════════════════════════════════════════════════════════════
 * MakeDeferredPromise
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(XjsObjectTest, MakeDeferredPromiseReturnsPromise) {
  xJSObjectRef resolve = nullptr;
  xJSObjectRef reject  = nullptr;
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, &resolve, &reject, nullptr);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(resolve, nullptr);
  ASSERT_NE(reject, nullptr);

  /* p instanceof Promise, and resolve/reject are functions. */
  xJSStringRef kP = xJSStringCreateWithUTF8CString("__p");
  xJSStringRef kR = xJSStringCreateWithUTF8CString("__r");
  xJSStringRef kJ = xJSStringCreateWithUTF8CString("__j");
  xJSObjectRef g  = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, kP, (xJSValueRef)p,
                       kXJSPropertyAttributeNone, nullptr);
  xJSObjectSetProperty(ctx_, g, kR, (xJSValueRef)resolve,
                       kXJSPropertyAttributeNone, nullptr);
  xJSObjectSetProperty(ctx_, g, kJ, (xJSValueRef)reject,
                       kXJSPropertyAttributeNone, nullptr);

  xJSStringRef src = xJSStringCreateWithUTF8CString(
    "(__p instanceof Promise) && (typeof __r === 'function') && "
    "(typeof __j === 'function')");
  xJSValueRef r = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(xJSValueToBoolean(ctx_, r));

  xjs_slot_release(r);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)g);
  xJSStringRelease(kP);
  xJSStringRelease(kR);
  xJSStringRelease(kJ);
  xjs_slot_release((xJSValueRef)resolve);
  xjs_slot_release((xJSValueRef)reject);
  xjs_slot_release((xJSValueRef)p);
}

TEST_F(XjsObjectTest, MakeDeferredPromiseResolveIsCallable) {
  xJSObjectRef resolve = nullptr;
  xJSObjectRef reject  = nullptr;
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, &resolve, &reject, nullptr);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(resolve, nullptr);

  /* Call resolve(42) as a host-side function: must succeed and
   * return undefined per spec. */
  xJSValueRef arg42  = xJSValueMakeNumber(ctx_, 42);
  xJSValueRef args[] = {arg42};
  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, resolve, nullptr, 1, args, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_TRUE(xJSValueIsUndefined(ctx_, rv));

  xjs_slot_release(rv);
  xjs_slot_release(arg42);
  xjs_slot_release((xJSValueRef)resolve);
  xjs_slot_release((xJSValueRef)reject);
  xjs_slot_release((xJSValueRef)p);
}

TEST_F(XjsObjectTest, MakeDeferredPromiseRejectIsCallable) {
  xJSObjectRef resolve = nullptr;
  xJSObjectRef reject  = nullptr;
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, &resolve, &reject, nullptr);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(reject, nullptr);

  xJSValueRef err    = xJSValueMakeNumber(ctx_, -1);
  xJSValueRef args[] = {err};
  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, reject, nullptr, 1, args, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_TRUE(xJSValueIsUndefined(ctx_, rv));

  xjs_slot_release(rv);
  xjs_slot_release(err);
  xjs_slot_release((xJSValueRef)resolve);
  xjs_slot_release((xJSValueRef)reject);
  xjs_slot_release((xJSValueRef)p);
}

TEST_F(XjsObjectTest, MakeDeferredPromiseResolveDeliversValue) {
  xJSObjectRef resolve = nullptr;
  xJSObjectRef reject  = nullptr;
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, &resolve, &reject, nullptr);
  ASSERT_NE(p, nullptr);

  /* Expose the promise to JS and register a .then handler that
   * stashes the resolved value on a global. */
  xJSStringRef kP = xJSStringCreateWithUTF8CString("__p");
  xJSObjectRef g  = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, kP, (xJSValueRef)p,
                       kXJSPropertyAttributeNone, nullptr);
  xJSStringRef setup = xJSStringCreateWithUTF8CString(
    "globalThis.__out = null; __p.then(v => { globalThis.__out = v; });");
  xJSValueRef r0 =
    xJSEvaluateScript(ctx_, setup, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r0, nullptr);
  xjs_slot_release(r0);
  xJSStringRelease(setup);

  /* Fire resolve(42) from the host side. */
  xJSValueRef arg42  = xJSValueMakeNumber(ctx_, 42);
  xJSValueRef args[] = {arg42};
  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, resolve, nullptr, 1, args, nullptr);
  ASSERT_NE(rv, nullptr);
  xjs_slot_release(rv);
  xjs_slot_release(arg42);

  /* Drain the microtask queue to run the .then callback. */
  xJSContextDrainPendingJobs(ctx_, nullptr);

  xJSStringRef probe =
    xJSStringCreateWithUTF8CString("globalThis.__out");
  xJSValueRef out =
    xJSEvaluateScript(ctx_, probe, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, out, nullptr), 42.0);
  xjs_slot_release(out);
  xJSStringRelease(probe);

  xJSStringRelease(kP);
  xjs_slot_release((xJSValueRef)g);
  xjs_slot_release((xJSValueRef)resolve);
  xjs_slot_release((xJSValueRef)reject);
  xjs_slot_release((xJSValueRef)p);
}

TEST_F(XjsObjectTest, MakeDeferredPromiseRejectDeliversReason) {
  xJSObjectRef resolve = nullptr;
  xJSObjectRef reject  = nullptr;
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, &resolve, &reject, nullptr);
  ASSERT_NE(p, nullptr);

  xJSStringRef kP = xJSStringCreateWithUTF8CString("__p");
  xJSObjectRef g  = xJSContextGetGlobalObject(ctx_);
  xJSObjectSetProperty(ctx_, g, kP, (xJSValueRef)p,
                       kXJSPropertyAttributeNone, nullptr);
  xJSStringRef setup = xJSStringCreateWithUTF8CString(
    "globalThis.__why = null; __p.catch(e => { globalThis.__why = e; });");
  xJSValueRef r0 =
    xJSEvaluateScript(ctx_, setup, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r0, nullptr);
  xjs_slot_release(r0);
  xJSStringRelease(setup);

  xJSValueRef neg7   = xJSValueMakeNumber(ctx_, -7);
  xJSValueRef args[] = {neg7};
  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, reject, nullptr, 1, args, nullptr);
  ASSERT_NE(rv, nullptr);
  xjs_slot_release(rv);
  xjs_slot_release(neg7);

  xJSContextDrainPendingJobs(ctx_, nullptr);

  xJSStringRef probe =
    xJSStringCreateWithUTF8CString("globalThis.__why");
  xJSValueRef out =
    xJSEvaluateScript(ctx_, probe, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, out, nullptr), -7.0);
  xjs_slot_release(out);
  xJSStringRelease(probe);

  xJSStringRelease(kP);
  xjs_slot_release((xJSValueRef)g);
  xjs_slot_release((xJSValueRef)resolve);
  xjs_slot_release((xJSValueRef)reject);
  xjs_slot_release((xJSValueRef)p);
}

TEST_F(XjsObjectTest, MakeDeferredPromiseNullOutParamsAreSafe) {
  /* If the caller doesn't ask for resolve/reject, the slots should
   * still be released (no leak, no crash).  */
  xJSObjectRef p =
    xJSObjectMakeDeferredPromise(ctx_, nullptr, nullptr, nullptr);
  ASSERT_NE(p, nullptr);
  xjs_slot_release((xJSValueRef)p);
}

/* ═══════════════════════════════════════════════════════════════════
 * MakeFunction (from source)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(XjsObjectTest, MakeFunctionZeroArgsBody) {
  xJSStringRef body = xJSStringCreateWithUTF8CString("return 7;");
  xJSObjectRef fn =
    xJSObjectMakeFunction(ctx_, nullptr, 0, nullptr, body, nullptr, 0, nullptr);
  ASSERT_NE(fn, nullptr);

  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, fn, nullptr, 0, nullptr, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, rv, nullptr), 7.0);

  xjs_slot_release(rv);
  xjs_slot_release((xJSValueRef)fn);
  xJSStringRelease(body);
}

TEST_F(XjsObjectTest, MakeFunctionWithNameAndParams) {
  xJSStringRef name = xJSStringCreateWithUTF8CString("add");
  xJSStringRef a    = xJSStringCreateWithUTF8CString("a");
  xJSStringRef b    = xJSStringCreateWithUTF8CString("b");
  xJSStringRef body = xJSStringCreateWithUTF8CString("return a + b;");
  const xJSStringRef ps[] = {a, b};

  xJSObjectRef fn =
    xJSObjectMakeFunction(ctx_, name, 2, ps, body, nullptr, 0, nullptr);
  ASSERT_NE(fn, nullptr);

  /* Call add(3, 4) → 7. */
  xJSValueRef v3 = xJSValueMakeNumber(ctx_, 3);
  xJSValueRef v4 = xJSValueMakeNumber(ctx_, 4);
  xJSValueRef args[] = {v3, v4};
  xJSValueRef rv =
    xJSObjectCallAsFunction(ctx_, fn, nullptr, 2, args, nullptr);
  ASSERT_NE(rv, nullptr);
  EXPECT_EQ(xJSValueToNumber(ctx_, rv, nullptr), 7.0);
  xjs_slot_release(rv);
  xjs_slot_release(v3);
  xjs_slot_release(v4);

  /* .name should be "add" */
  xJSStringRef kName = xJSStringCreateWithUTF8CString("name");
  xJSValueRef  nv = xJSObjectGetProperty(ctx_, fn, kName, nullptr);
  ASSERT_NE(nv, nullptr);
  xJSStringRef ns = xJSValueToStringCopy(ctx_, nv, nullptr);
  char         buf[16] = {0};
  xJSStringGetUTF8CString(ns, buf, sizeof(buf));
  EXPECT_STREQ(buf, "add");
  xJSStringRelease(ns);
  xjs_slot_release(nv);
  xJSStringRelease(kName);

  xjs_slot_release((xJSValueRef)fn);
  xJSStringRelease(body);
  xJSStringRelease(a);
  xJSStringRelease(b);
  xJSStringRelease(name);
}

TEST_F(XjsObjectTest, MakeFunctionSyntaxErrorPropagates) {
  xJSStringRef body = xJSStringCreateWithUTF8CString("return @@@;");
  xJSValueRef  exc  = nullptr;
  xJSObjectRef fn =
    xJSObjectMakeFunction(ctx_, nullptr, 0, nullptr, body, nullptr, 0, &exc);
  EXPECT_EQ(fn, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xJSStringRelease(body);
}

TEST_F(XjsObjectTest, MakeFunctionNullBodyReturnsNull) {
  EXPECT_EQ(
    xJSObjectMakeFunction(ctx_, nullptr, 0, nullptr, nullptr, nullptr, 0,
                          nullptr),
    nullptr);
}
