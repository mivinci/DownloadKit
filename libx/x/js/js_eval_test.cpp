/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_eval_test.cpp - Script syntax check, evaluation, and GC.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

namespace {
class XjsEvalTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = xJSGlobalContextCreate(nullptr);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) xJSGlobalContextRelease(ctx_);
  }
  xJSGlobalContextRef ctx_ = nullptr;
};
} // namespace

/* ─────────── CheckScriptSyntax ─────────── */

TEST_F(XjsEvalTest, CheckSyntaxOk) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("1 + 2");
  EXPECT_TRUE(xJSCheckScriptSyntax(ctx_, s, nullptr, 1, nullptr));
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, CheckSyntaxError) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("function (");
  xJSValueRef  exc = nullptr;
  EXPECT_FALSE(xJSCheckScriptSyntax(ctx_, s, nullptr, 1, &exc));
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, CheckSyntaxErrorNoExcOutIsOk) {
  xJSStringRef s = xJSStringCreateWithUTF8CString(")))");
  EXPECT_FALSE(xJSCheckScriptSyntax(ctx_, s, nullptr, 1, nullptr));
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, CheckSyntaxNullScript) {
  EXPECT_FALSE(xJSCheckScriptSyntax(ctx_, nullptr, nullptr, 1, nullptr));
}

TEST_F(XjsEvalTest, CheckSyntaxWithSourceUrl) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("42");
  xJSStringRef url = xJSStringCreateWithUTF8CString("file:///tmp/a.js");
  EXPECT_TRUE(xJSCheckScriptSyntax(ctx_, s, url, 1, nullptr));
  xJSStringRelease(s);
  xJSStringRelease(url);
}

/* ─────────── EvaluateScript ─────────── */

TEST_F(XjsEvalTest, EvaluateReturnsValue) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("40 + 2");
  xJSValueRef  r = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 1, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(xJSValueIsNumber(ctx_, r));
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r, nullptr), 42.0);
  xjs_slot_release(r);
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, EvaluateString) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("'he' + 'llo'");
  xJSValueRef  r = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 1, nullptr);
  ASSERT_NE(r, nullptr);
  xJSStringRef out = xJSValueToStringCopy(ctx_, r, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(out, "hello"));
  xJSStringRelease(out);
  xjs_slot_release(r);
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, EvaluateRuntimeErrorPropagates) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("throw new Error('oops')");
  xJSValueRef  exc = nullptr;
  xJSValueRef  r   = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 1, &exc);
  EXPECT_EQ(r, nullptr);
  ASSERT_NE(exc, nullptr);
  xJSStringRef m    = xJSStringCreateWithUTF8CString("message");
  xJSValueRef  msg  = xJSObjectGetProperty(ctx_, (xJSObjectRef)exc, m, nullptr);
  xJSStringRef mstr = xJSValueToStringCopy(ctx_, msg, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(mstr, "oops"));
  xJSStringRelease(mstr);
  xjs_slot_release(msg);
  xJSStringRelease(m);
  xjs_slot_release(exc);
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, EvaluateSyntaxErrorPropagates) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("function (");
  xJSValueRef  exc = nullptr;
  xJSValueRef  r   = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 1, &exc);
  EXPECT_EQ(r, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
  xJSStringRelease(s);
}

TEST_F(XjsEvalTest, EvaluateNullScript) {
  EXPECT_EQ(xJSEvaluateScript(ctx_, nullptr, nullptr, nullptr, 0, nullptr), nullptr);
}

TEST_F(XjsEvalTest, EvaluateWithSourceUrl) {
  xJSStringRef s   = xJSStringCreateWithUTF8CString("1");
  xJSStringRef url = xJSStringCreateWithUTF8CString("inline.js");
  xJSValueRef  r   = xJSEvaluateScript(ctx_, s, nullptr, url, 1, nullptr);
  ASSERT_NE(r, nullptr);
  xjs_slot_release(r);
  xJSStringRelease(s);
  xJSStringRelease(url);
}

TEST_F(XjsEvalTest, EvaluatePopulatesGlobal) {
  /* Two evaluations should share the same global context. */
  xJSStringRef s1 = xJSStringCreateWithUTF8CString("globalThis.x = 7");
  xJSStringRef s2 = xJSStringCreateWithUTF8CString("x * 6");
  xJSValueRef  r1 = xJSEvaluateScript(ctx_, s1, nullptr, nullptr, 1, nullptr);
  ASSERT_NE(r1, nullptr);
  xjs_slot_release(r1);
  xJSValueRef r2 = xJSEvaluateScript(ctx_, s2, nullptr, nullptr, 1, nullptr);
  ASSERT_NE(r2, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r2, nullptr), 42.0);
  xjs_slot_release(r2);
  xJSStringRelease(s1);
  xJSStringRelease(s2);
}

/* ─────────── EvaluateScript: thisObject binding ─────────── */

TEST_F(XjsEvalTest, EvaluateWithThisObjectBindsThis) {
  /* Build a host-side object with a property we can observe from JS. */
  xJSObjectRef thiz = xJSObjectMake(ctx_, nullptr, nullptr);
  xJSStringRef kTag = xJSStringCreateWithUTF8CString("tag");
  xJSStringRef sTag = xJSStringCreateWithUTF8CString("hello");
  xJSValueRef  vTag = xJSValueMakeString(ctx_, sTag);
  xJSObjectSetProperty(ctx_, thiz, kTag, vTag, kXJSPropertyAttributeNone, nullptr);
  xjs_slot_release(vTag);
  xJSStringRelease(sTag);
  xJSStringRelease(kTag);

  /* Script reads `this.tag`.  With our xJSObjectRef pinned as this,
   * the result should be "hello" even though that property lives
   * nowhere on the global object. */
  xJSStringRef src = xJSStringCreateWithUTF8CString("this.tag");
  xJSValueRef  r   = xJSEvaluateScript(ctx_, src, thiz, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  xJSStringRef out = xJSValueToStringCopy(ctx_, r, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(out, "hello"));

  xJSStringRelease(out);
  xjs_slot_release(r);
  xJSStringRelease(src);
  xjs_slot_release((xJSValueRef)thiz);
}

TEST_F(XjsEvalTest, EvaluateNullThisObjectDefaultsToGlobal) {
  /* Null thisObject must fall back to the global object — same
   * behaviour as the pre-this-object overload. */
  xJSStringRef s1 = xJSStringCreateWithUTF8CString("globalThis.beacon = 'g';");
  xJSValueRef  r1 = xJSEvaluateScript(ctx_, s1, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r1, nullptr);
  xjs_slot_release(r1);
  xJSStringRelease(s1);

  xJSStringRef s2 = xJSStringCreateWithUTF8CString("this.beacon");
  xJSValueRef  r2 = xJSEvaluateScript(ctx_, s2, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r2, nullptr);
  xJSStringRef out = xJSValueToStringCopy(ctx_, r2, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(out, "g"));

  xJSStringRelease(out);
  xjs_slot_release(r2);
  xJSStringRelease(s2);
}

/* ─────────── GC ─────────── */

TEST_F(XjsEvalTest, GarbageCollectRuns) {
  /* Allocate some garbage first so GC has something to do. */
  xJSStringRef s = xJSStringCreateWithUTF8CString(
    "var arr = []; for (var i=0;i<100;++i) arr.push({i:i}); arr = null;");
  xJSValueRef r = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 1, nullptr);
  ASSERT_NE(r, nullptr);
  xjs_slot_release(r);
  xJSStringRelease(s);

  xJSGarbageCollect(ctx_); /* must not crash */
}

TEST_F(XjsEvalTest, GarbageCollectNullCtxIsNoOp) {
  xJSGarbageCollect(nullptr);
}

/* ─────────── Drain pending microtasks ─────────── */

TEST_F(XjsEvalTest, DrainPendingJobsNullCtxIsZero) {
  EXPECT_EQ(xJSContextDrainPendingJobs(nullptr, nullptr), 0);
  EXPECT_FALSE(xJSContextHasPendingJobs(nullptr));
}

TEST_F(XjsEvalTest, DrainPendingJobsNoneQueuedReturnsZero) {
  EXPECT_FALSE(xJSContextHasPendingJobs(ctx_));
  EXPECT_EQ(xJSContextDrainPendingJobs(ctx_, nullptr), 0);
}

TEST_F(XjsEvalTest, DrainPendingJobsRunsPromiseThenCallbacks) {
  /* Queue a microtask via Promise.resolve().then(…). */
  xJSStringRef src =
    xJSStringCreateWithUTF8CString("globalThis.__hit = 0; "
                                   "Promise.resolve(41).then(v => { globalThis.__hit = v + 1; });");
  xJSValueRef r = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  xjs_slot_release(r);
  xJSStringRelease(src);

  /* The .then callback is pending — verify it's queued. */
  EXPECT_TRUE(xJSContextHasPendingJobs(ctx_));

  int drained = xJSContextDrainPendingJobs(ctx_, nullptr);
  EXPECT_GE(drained, 1);
  EXPECT_FALSE(xJSContextHasPendingJobs(ctx_));

  /* Now __hit should be 42. */
  xJSStringRef probe = xJSStringCreateWithUTF8CString("globalThis.__hit");
  xJSValueRef  v     = xJSEvaluateScript(ctx_, probe, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 42.0);
  xjs_slot_release(v);
  xJSStringRelease(probe);
}

TEST_F(XjsEvalTest, DrainPendingJobsRunsAsyncAwaitContinuation) {
  /* async/await also queues microtasks — covers the other common
   * source of pending jobs. */
  xJSStringRef src = xJSStringCreateWithUTF8CString(
    "globalThis.__x = 0; "
    "(async () => { globalThis.__x = await Promise.resolve(99); })();");
  xJSValueRef r = xJSEvaluateScript(ctx_, src, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  xjs_slot_release(r);
  xJSStringRelease(src);

  EXPECT_TRUE(xJSContextHasPendingJobs(ctx_));
  xJSContextDrainPendingJobs(ctx_, nullptr);
  EXPECT_FALSE(xJSContextHasPendingJobs(ctx_));

  xJSStringRef probe = xJSStringCreateWithUTF8CString("globalThis.__x");
  xJSValueRef  v     = xJSEvaluateScript(ctx_, probe, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 99.0);
  xjs_slot_release(v);
  xJSStringRelease(probe);
}
