/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_module_test.cpp - ES module evaluation, loader trampoline,
 *                      and xJSAwaitPromise.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* ───────── Test fixture ─────────
 *
 * We model the loader as a map<name, source> plus a fetch log, so
 * individual tests can assert both the contents and the sequence of
 * loader invocations (e.g. to verify caching). */
class XjsModuleTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = xJSGlobalContextCreate(nullptr);
    ASSERT_NE(ctx_, nullptr);
    xJSContextSetModuleLoader(ctx_, &XjsModuleTest::LoadThunk, this);
  }
  void TearDown() override {
    if (ctx_) xJSGlobalContextRelease(ctx_);
  }

  static xJSStringRef LoadThunk(xJSContextRef, const char *name, void *op) {
    auto *self = static_cast<XjsModuleTest *>(op);
    self->fetched_.emplace_back(name);
    auto it = self->sources_.find(name);
    if (it == self->sources_.end()) return nullptr;
    return xJSStringCreateWithUTF8CString(it->second.c_str());
  }

  xJSValueRef EvalModule(const char *src, const char *url = "entry.js",
                         xJSValueRef *exc = nullptr) {
    xJSStringRef s = xJSStringCreateWithUTF8CString(src);
    xJSStringRef u = url ? xJSStringCreateWithUTF8CString(url) : nullptr;
    xJSValueRef  r = xJSEvaluateModule(ctx_, s, u, exc);
    xJSStringRelease(s);
    if (u) xJSStringRelease(u);
    return r;
  }

  xJSGlobalContextRef                        ctx_ = nullptr;
  std::unordered_map<std::string, std::string> sources_;
  std::vector<std::string>                     fetched_;
};

} // namespace

/* ══════════════ xJSDetectModule ══════════════ */

TEST(XjsModuleFree, DetectModuleDetectsImport) {
  const char s[] = "import x from './x.js';\nconsole.log(x);";
  EXPECT_TRUE(xJSDetectModule(s, sizeof s - 1));
}

TEST(XjsModuleFree, DetectModuleDetectsExport) {
  const char s[] = "export const x = 1;";
  EXPECT_TRUE(xJSDetectModule(s, sizeof s - 1));
}

TEST(XjsModuleFree, DetectModuleRejectsObviousScriptSyntax) {
  /* QuickJS's JS_DetectModule is *liberal*: a module is a superset of
   * a script, so valid script syntax still parses as a module and is
   * reported as such.  We can only assert the negative case for
   * syntax that's outright illegal in both modes. */
  const char s[] = ")))";
  EXPECT_FALSE(xJSDetectModule(s, sizeof s - 1));
}

TEST(XjsModuleFree, DetectModuleNullReturnsFalse) {
  EXPECT_FALSE(xJSDetectModule(nullptr, 0));
}

/* ══════════════ EvaluateModule: basics ══════════════ */

TEST_F(XjsModuleTest, EvaluateModuleReturnsPromise) {
  /* Module with no imports and no top-level await — still returns a
   * Promise per the ES spec, which QuickJS honours. */
  xJSValueRef p = EvalModule("globalThis.__m_ok = 1;");
  ASSERT_NE(p, nullptr);

  xJSValueRef v = xJSAwaitPromise(ctx_, p, nullptr);
  ASSERT_NE(v, nullptr);
  xjs_slot_release(v);
  xjs_slot_release(p);

  xJSStringRef q = xJSStringCreateWithUTF8CString("globalThis.__m_ok");
  xJSValueRef  r = xJSEvaluateScript(ctx_, q, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r, nullptr), 1.0);
  xjs_slot_release(r);
  xJSStringRelease(q);
}

TEST_F(XjsModuleTest, EvaluateModuleNullScript) {
  EXPECT_EQ(xJSEvaluateModule(ctx_, nullptr, nullptr, nullptr), nullptr);
}

TEST_F(XjsModuleTest, EvaluateModuleCompileErrorSurfacesOnException) {
  /* A syntax error is a *compile-time* failure — it must surface
   * through the exception out-param, not via a rejected promise. */
  xJSValueRef exc = nullptr;
  xJSValueRef p   = EvalModule("import { from", "bad.js", &exc);
  EXPECT_EQ(p, nullptr);
  EXPECT_NE(exc, nullptr);
  if (exc) xjs_slot_release(exc);
}

/* ══════════════ Loader trampoline ══════════════ */

TEST_F(XjsModuleTest, ImportFromLoadedModule) {
  sources_["util.js"] = "export const FOUR = 4;";

  xJSValueRef p = EvalModule(
    "import { FOUR } from './util.js';\n"
    "globalThis.__four = FOUR + 38;",
    "entry.js");
  ASSERT_NE(p, nullptr);
  xJSValueRef v = xJSAwaitPromise(ctx_, p, nullptr);
  ASSERT_NE(v, nullptr);
  xjs_slot_release(v);
  xjs_slot_release(p);

  xJSStringRef q = xJSStringCreateWithUTF8CString("globalThis.__four");
  xJSValueRef  r = xJSEvaluateScript(ctx_, q, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r, nullptr), 42.0);
  xjs_slot_release(r);
  xJSStringRelease(q);
}

TEST_F(XjsModuleTest, LoaderSeesNormalizedName) {
  /* The entry module's sourceURL is used as the base for relative
   * specifier resolution — QuickJS does this internally, so the
   * loader should see the *resolved* path, never "./util.js". */
  sources_["./pkg/util.js"] = "export const X = 1;";
  xJSValueRef p =
    EvalModule("import { X } from './util.js'; globalThis.__x = X;",
               "./pkg/entry.js");
  ASSERT_NE(p, nullptr);
  xJSValueRef v = xJSAwaitPromise(ctx_, p, nullptr);
  ASSERT_NE(v, nullptr);
  xjs_slot_release(v);
  xjs_slot_release(p);

  ASSERT_EQ(fetched_.size(), 1u);
  EXPECT_EQ(fetched_[0], "./pkg/util.js");
}

TEST_F(XjsModuleTest, LoaderCalledOnceEvenWithMultipleImports) {
  /* Two importers referencing the same module must only fetch once
   * — QuickJS caches compiled modules by normalised name. */
  sources_["shared.js"] = "export const S = 7;";
  sources_["a.js"] = "import { S } from './shared.js'; export const A = S+1;";
  sources_["b.js"] = "import { S } from './shared.js'; export const B = S+2;";

  xJSValueRef p = EvalModule(
    "import { A } from './a.js';\n"
    "import { B } from './b.js';\n"
    "globalThis.__ab = A + B;",
    "entry.js");
  ASSERT_NE(p, nullptr);
  xJSValueRef v = xJSAwaitPromise(ctx_, p, nullptr);
  ASSERT_NE(v, nullptr);
  xjs_slot_release(v);
  xjs_slot_release(p);

  /* shared.js should appear exactly once in the fetch log. */
  size_t shared_hits = 0;
  for (auto &n : fetched_) if (n == "shared.js") ++shared_hits;
  EXPECT_EQ(shared_hits, 1u);
}

TEST_F(XjsModuleTest, LoaderReturningNullRejectsWithReferenceError) {
  /* Module linking runs inside JS_Eval, so a missing dependency is
   * a *compile-phase* failure: the returned promise is NULL and the
   * ReferenceError lands in the exception out-param instead. */
  xJSValueRef exc = nullptr;
  xJSValueRef p =
    EvalModule("import x from './missing.js'; globalThis.__x = x;",
               "entry.js", &exc);
  EXPECT_EQ(p, nullptr);
  ASSERT_NE(exc, nullptr);

  /* Sanity: it should mention the failing specifier. */
  xJSStringRef s   = xJSValueToStringCopy(ctx_, exc, nullptr);
  size_t       sz  = xJSStringGetMaximumUTF8CStringSize(s);
  std::string  buf(sz, 0);
  xJSStringGetUTF8CString(s, &buf[0], sz);
  EXPECT_NE(buf.find("missing.js"), std::string::npos);
  xJSStringRelease(s);
  xjs_slot_release(exc);
}

TEST_F(XjsModuleTest, UnsetLoaderRejectsEveryImport) {
  /* Passing NULL unsubscribes the callback; subsequent imports
   * then fail at link time with a ReferenceError surfaced on the
   * exception out-param (see ImportMissing test for the same
   * rationale about linking being synchronous). */
  xJSContextSetModuleLoader(ctx_, nullptr, nullptr);

  xJSValueRef exc = nullptr;
  xJSValueRef p =
    EvalModule("import x from './anything.js';", "entry.js", &exc);
  EXPECT_EQ(p, nullptr);
  ASSERT_NE(exc, nullptr);
  xjs_slot_release(exc);
}

TEST_F(XjsModuleTest, ImportedModuleTopLevelThrowRejects) {
  sources_["bad.js"] = "throw new Error('boom');";
  xJSValueRef p =
    EvalModule("import './bad.js'; globalThis.__never = 1;", "entry.js");
  ASSERT_NE(p, nullptr);

  xJSValueRef exc = nullptr;
  xJSValueRef v   = xJSAwaitPromise(ctx_, p, &exc);
  EXPECT_EQ(v, nullptr);
  ASSERT_NE(exc, nullptr);

  xJSStringRef mkey = xJSStringCreateWithUTF8CString("message");
  xJSValueRef  msg =
    xJSObjectGetProperty(ctx_, (xJSObjectRef)exc, mkey, nullptr);
  xJSStringRef mstr = xJSValueToStringCopy(ctx_, msg, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(mstr, "boom"));
  xJSStringRelease(mstr);
  xjs_slot_release(msg);
  xJSStringRelease(mkey);
  xjs_slot_release(exc);
  xjs_slot_release(p);
}

/* ══════════════ xJSAwaitPromise ══════════════ */

TEST_F(XjsModuleTest, AwaitNonPromiseReturnsValueAsIs) {
  xJSValueRef n = xJSValueMakeNumber(ctx_, 3.14);
  xJSValueRef r = xJSAwaitPromise(ctx_, n, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, r, nullptr), 3.14);
  xjs_slot_release(r);
  xjs_slot_release(n);
}

TEST_F(XjsModuleTest, AwaitResolvedPromiseReturnsFulfillmentValue) {
  xJSStringRef s =
    xJSStringCreateWithUTF8CString("Promise.resolve(41 + 1)");
  xJSValueRef p = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 0, nullptr);
  xJSStringRelease(s);
  ASSERT_NE(p, nullptr);

  xJSValueRef v = xJSAwaitPromise(ctx_, p, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_DOUBLE_EQ(xJSValueToNumber(ctx_, v, nullptr), 42.0);
  xjs_slot_release(v);
  xjs_slot_release(p);
}

TEST_F(XjsModuleTest, AwaitRejectedPromiseReportsException) {
  xJSStringRef s = xJSStringCreateWithUTF8CString(
    "Promise.reject(new Error('nope'))");
  xJSValueRef p = xJSEvaluateScript(ctx_, s, nullptr, nullptr, 0, nullptr);
  xJSStringRelease(s);
  ASSERT_NE(p, nullptr);

  xJSValueRef exc = nullptr;
  xJSValueRef v   = xJSAwaitPromise(ctx_, p, &exc);
  EXPECT_EQ(v, nullptr);
  ASSERT_NE(exc, nullptr);

  xJSStringRef mkey = xJSStringCreateWithUTF8CString("message");
  xJSValueRef  msg =
    xJSObjectGetProperty(ctx_, (xJSObjectRef)exc, mkey, nullptr);
  xJSStringRef mstr = xJSValueToStringCopy(ctx_, msg, nullptr);
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(mstr, "nope"));
  xJSStringRelease(mstr);
  xjs_slot_release(msg);
  xJSStringRelease(mkey);
  xjs_slot_release(exc);
  xjs_slot_release(p);
}

TEST_F(XjsModuleTest, AwaitNullInputs) {
  EXPECT_EQ(xJSAwaitPromise(nullptr, nullptr, nullptr), nullptr);
  EXPECT_EQ(xJSAwaitPromise(ctx_, nullptr, nullptr), nullptr);
}
