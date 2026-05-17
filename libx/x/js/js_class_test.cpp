/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_class_test.cpp - end-to-end coverage for xJSClassCreate and the
 * class <-> object private-data plumbing.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

/* ─── Minimal class factory used by several tests ─── */

namespace {

/* Static counters live at file scope so finalize callbacks (which run
 * during GC, possibly after the TEST body returns) can still record. */
std::atomic<int> g_initialize_count{0};
std::atomic<int> g_finalize_count{0};

void reset_counts() {
  g_initialize_count = 0;
  g_finalize_count   = 0;
}

void on_initialize(xJSContextRef /*ctx*/, xJSObjectRef /*obj*/) {
  g_initialize_count.fetch_add(1);
}

void on_finalize(xJSObjectRef obj) {
  g_finalize_count.fetch_add(1);
  /* Touch the private slot so any double-free / stale pointer would
   * be caught by the sanitiser. */
  (void)xJSObjectGetPrivate(obj);
}

xJSClassRef make_counting_class(const char *name = "Counter") {
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = name;
  def.initialize         = on_initialize;
  def.finalize           = on_finalize;
  return xJSClassCreate(&def);
}

} // namespace

/* ═══════════════════════════════════════════════════════════════════
 * Create / Retain / Release
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsClass, CreateRejectsNull) {
  EXPECT_EQ(xJSClassCreate(nullptr), nullptr);
}

TEST(XjsClass, CreateAllocatesDistinctClassIDs) {
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = "A";
  xJSClassRef a          = xJSClassCreate(&def);
  def.className          = "B";
  xJSClassRef b          = xJSClassCreate(&def);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  /* Class ids are allocated lazily on first ensure-registered (quickjs-ng
   * class ids are per-runtime), so pre-registration both ids read as 0. */
  EXPECT_NE(a, b);
  EXPECT_EQ(a->qclass, 0u);
  EXPECT_EQ(b->qclass, 0u);
  /* Instantiating one object of each class forces registration; the
   * two ids must then be distinct non-zero values. */
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  int          dummy = 0;
  xJSObjectRef oa    = xJSObjectMake(ctx, a, &dummy);
  xJSObjectRef ob    = xJSObjectMake(ctx, b, &dummy);
  ASSERT_NE(oa, nullptr);
  ASSERT_NE(ob, nullptr);
  EXPECT_NE(a->qclass, 0u);
  EXPECT_NE(b->qclass, 0u);
  EXPECT_NE(a->qclass, b->qclass);
  xJSValueUnprotect(ctx, (xJSValueRef)oa);
  xJSValueUnprotect(ctx, (xJSValueRef)ob);
  xJSGlobalContextRelease(ctx);
  xJSClassRelease(a);
  xJSClassRelease(b);
}

TEST(XjsClass, CreateDuplicatesClassName) {
  /* The JSC contract lets callers pass a stack-owned className; we
   * must not alias it. */
  char stackbuf[8];
  std::strcpy(stackbuf, "OnStack");
  xJSClassDefinition def = kXJSClassDefinitionEmpty;
  def.className          = stackbuf;
  xJSClassRef k          = xJSClassCreate(&def);
  ASSERT_NE(k, nullptr);
  std::memset(stackbuf, 'x', sizeof(stackbuf)); /* clobber caller buffer */
  ASSERT_NE(k->class_name, nullptr);
  EXPECT_STREQ(k->class_name, "OnStack");
  xJSClassRelease(k);
}

TEST(XjsClass, RetainReleaseRefcount) {
  xJSClassRef k = make_counting_class();
  ASSERT_NE(k, nullptr);
  EXPECT_EQ(k->refcount, 1);
  EXPECT_EQ(xJSClassRetain(k), k);
  EXPECT_EQ(k->refcount, 2);
  xJSClassRelease(k);
  EXPECT_EQ(k->refcount, 1);
  xJSClassRelease(k);
}

TEST(XjsClass, NullSafeLifecycle) {
  EXPECT_EQ(xJSClassRetain(nullptr), nullptr);
  xJSClassRelease(nullptr); /* must not crash */
}

/* ═══════════════════════════════════════════════════════════════════
 * xJSObjectMake + private-data round-trip
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsClass, ObjectMakeWithoutClassIgnoresPrivateData) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  int          sentinel = 42;
  xJSObjectRef o        = xJSObjectMake(ctx, nullptr, &sentinel);
  ASSERT_NE(o, nullptr);
  /* JSC contract: private data on a plain object is not stored. */
  EXPECT_EQ(xJSObjectGetPrivate(o), nullptr);
  /* Setting on a classless object must fail rather than corrupt. */
  EXPECT_FALSE(xJSObjectSetPrivate(o, &sentinel));

  xjs_slot_release((xJSValueRef)o);
  xJSGlobalContextRelease(ctx);
}

TEST(XjsClass, ObjectMakeWithClassRoundTripsPrivateData) {
  reset_counts();
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  xJSClassRef cls = make_counting_class();
  ASSERT_NE(cls, nullptr);

  int          sentinel = 0xC0FFEE;
  xJSObjectRef o        = xJSObjectMake(ctx, cls, &sentinel);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(xJSObjectGetPrivate(o), &sentinel);
  EXPECT_EQ(g_initialize_count.load(), 1);

  int other = 7;
  EXPECT_TRUE(xJSObjectSetPrivate(o, &other));
  EXPECT_EQ(xJSObjectGetPrivate(o), &other);

  xjs_slot_release((xJSValueRef)o);
  xJSClassRelease(cls);
  xJSGlobalContextRelease(ctx);
  /* Finalize runs during context/runtime teardown (GC sweep). */
  EXPECT_GE(g_finalize_count.load(), 1);
}

TEST(XjsClass, SameClassUsableAcrossContexts) {
  /* A single xJSClassRef must work against multiple runtimes thanks
   * to xjs_class_ensure_registered()'s per-runtime idempotency. */
  reset_counts();
  xJSClassRef cls = make_counting_class("Shared");
  ASSERT_NE(cls, nullptr);

  for (int i = 0; i < 3; ++i) {
    xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
    ASSERT_NE(ctx, nullptr);
    int          v = i;
    xJSObjectRef o = xJSObjectMake(ctx, cls, &v);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(xJSObjectGetPrivate(o), &v);
    xjs_slot_release((xJSValueRef)o);
    xJSGlobalContextRelease(ctx);
  }
  xJSClassRelease(cls);
  EXPECT_EQ(g_initialize_count.load(), 3);
  EXPECT_GE(g_finalize_count.load(), 3);
}

TEST(XjsClass, GetPrivateRejectsNullObject) {
  EXPECT_EQ(xJSObjectGetPrivate(nullptr), nullptr);
  EXPECT_FALSE(xJSObjectSetPrivate(nullptr, nullptr));
}
