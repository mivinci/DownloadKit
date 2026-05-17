/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_context_test.cpp - xJSContextGroup / xJSGlobalContext lifecycle
 * and accessor tests.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

/* ═══════════════════════════════════════════════════════════════════
 * Context group
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsCtxGroup, CreateAndRelease) {
  xJSContextGroupRef g = xJSContextGroupCreate();
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->refcount, 1);
  EXPECT_NE(g->rt, nullptr);
  xJSContextGroupRelease(g);
}

TEST(XjsCtxGroup, RetainReleaseRefcount) {
  xJSContextGroupRef g = xJSContextGroupCreate();
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->refcount, 1);

  xJSContextGroupRef g2 = xJSContextGroupRetain(g);
  EXPECT_EQ(g2, g);
  EXPECT_EQ(g->refcount, 2);

  xJSContextGroupRelease(g);
  EXPECT_EQ(g->refcount, 1);
  xJSContextGroupRelease(g); /* frees */
}

TEST(XjsCtxGroup, NullSafe) {
  EXPECT_EQ(xJSContextGroupRetain(nullptr), nullptr);
  xJSContextGroupRelease(nullptr); /* no crash */
}

/* ═══════════════════════════════════════════════════════════════════
 * Global context
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsGlobalCtx, CreateStandalone) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->refcount, 1);
  EXPECT_NE(ctx->ctx, nullptr);
  ASSERT_NE(ctx->group, nullptr);
  /* Auto-created group is retained solely by the context. */
  EXPECT_EQ(ctx->group->refcount, 1);
  xJSGlobalContextRelease(ctx);
}

TEST(XjsGlobalCtx, CreateInGroupSharesHeap) {
  xJSContextGroupRef g = xJSContextGroupCreate();
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->refcount, 1);

  xJSGlobalContextRef a = xJSGlobalContextCreateInGroup(g, nullptr);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(g->refcount, 2); /* group retained by context */

  xJSGlobalContextRef b = xJSGlobalContextCreateInGroup(g, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(g->refcount, 3);
  EXPECT_EQ(a->group, g);
  EXPECT_EQ(b->group, g);

  xJSGlobalContextRelease(a);
  EXPECT_EQ(g->refcount, 2);
  xJSGlobalContextRelease(b);
  EXPECT_EQ(g->refcount, 1);
  xJSContextGroupRelease(g);
}

TEST(XjsGlobalCtx, CreateInGroupNullGroup) {
  EXPECT_EQ(xJSGlobalContextCreateInGroup(nullptr, nullptr), nullptr);
}

TEST(XjsGlobalCtx, RetainRelease) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->refcount, 1);

  xJSGlobalContextRef c2 = xJSGlobalContextRetain(ctx);
  EXPECT_EQ(c2, ctx);
  EXPECT_EQ(ctx->refcount, 2);

  xJSGlobalContextRelease(ctx);
  EXPECT_EQ(ctx->refcount, 1);
  xJSGlobalContextRelease(ctx); /* frees */
}

TEST(XjsGlobalCtx, RetainReleaseNullSafe) {
  EXPECT_EQ(xJSGlobalContextRetain(nullptr), nullptr);
  xJSGlobalContextRelease(nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * Accessors
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsGlobalCtx, GetGlobalObject) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  xJSObjectRef g = xJSContextGetGlobalObject(ctx);
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(xJSValueIsObject(ctx, (xJSValueRef)g));
  xjs_slot_release((xJSValueRef)g);

  xJSGlobalContextRelease(ctx);
}

TEST(XjsGlobalCtx, GetGlobalObjectNullCtx) {
  EXPECT_EQ(xJSContextGetGlobalObject(nullptr), nullptr);
}

TEST(XjsGlobalCtx, GetGroup) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(xJSContextGetGroup(ctx), ctx->group);
  EXPECT_EQ(xJSContextGetGroup(nullptr), nullptr);
  xJSGlobalContextRelease(ctx);
}

TEST(XjsGlobalCtx, GetGlobalContextReturnsSelf) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(xJSContextGetGlobalContext(ctx), ctx);
  xJSGlobalContextRelease(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Name accessors
 * ═══════════════════════════════════════════════════════════════════ */

TEST(XjsGlobalCtx, NameInitiallyNull) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(xJSGlobalContextCopyName(ctx), nullptr);
  xJSGlobalContextRelease(ctx);
}

TEST(XjsGlobalCtx, SetAndCopyName) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(nullptr);
  ASSERT_NE(ctx, nullptr);

  xJSStringRef name = xJSStringCreateWithUTF8CString("worker-1");
  xJSGlobalContextSetName(ctx, name);
  xJSStringRelease(name);

  xJSStringRef copy = xJSGlobalContextCopyName(ctx);
  ASSERT_NE(copy, nullptr);
  char buf[32] = {0};
  xJSStringGetUTF8CString(copy, buf, sizeof(buf));
  EXPECT_STREQ(buf, "worker-1");
  xJSStringRelease(copy);

  /* Overwriting should free the previous name. */
  xJSStringRef n2 = xJSStringCreateWithUTF8CString("worker-2");
  xJSGlobalContextSetName(ctx, n2);
  xJSStringRelease(n2);

  copy = xJSGlobalContextCopyName(ctx);
  ASSERT_NE(copy, nullptr);
  memset(buf, 0, sizeof(buf));
  xJSStringGetUTF8CString(copy, buf, sizeof(buf));
  EXPECT_STREQ(buf, "worker-2");
  xJSStringRelease(copy);

  /* Clearing by passing NULL. */
  xJSGlobalContextSetName(ctx, nullptr);
  EXPECT_EQ(xJSGlobalContextCopyName(ctx), nullptr);

  xJSGlobalContextRelease(ctx);
}

TEST(XjsGlobalCtx, SetNameNullCtxIsNoOp) {
  xJSGlobalContextSetName(nullptr, nullptr);
  EXPECT_EQ(xJSGlobalContextCopyName(nullptr), nullptr);
}
