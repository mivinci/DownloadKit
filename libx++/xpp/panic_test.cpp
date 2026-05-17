/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * panic_test.cpp - Tests for XPP_PANIC / XPP_ASSERT / XPP_DEBUG_ASSERT.
 *
 * Panics terminate the process, so we rely on gtest's death tests with
 * "threadsafe" style (re-exec rather than fork, ASan-safe).
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <xpp/panic.h>
#include <xpp/variant.h>

extern "C" {
#include <x/base/log.h>
}

/* ── XPP_PANIC ── */

TEST(PanicDeathTest, UnconditionalPanicAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH({ XPP_PANIC("boom"); }, "boom");
}

TEST(PanicDeathTest, PanicMessageIncludesFileAndLine) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  // The default xLog fallback prints "panic at <file>:<line>: <msg>".
  EXPECT_DEATH({ XPP_PANIC("custom message"); }, "panic at .*panic_test\\.cpp:.*custom message");
}

TEST(PanicDeathTest, PanicFormatsArguments) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
    { XPP_PANIC("idx %d out of range (size=%d)", 7, 4); }, "idx 7 out of range \\(size=4\\)");
}

/* ── XPP_ASSERT ── */

TEST(PanicTest, AssertPassesWhenTrue) {
  // Should not panic; reaches the next line.
  XPP_ASSERT(1 + 1 == 2, "math holds");
  SUCCEED();
}

TEST(PanicDeathTest, AssertAbortsWhenFalse) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH({ XPP_ASSERT(false, "should fail"); }, "assertion failed: false .* should fail");
}

TEST(PanicDeathTest, AssertIncludesStringifiedCondition) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  int x = 0;
  EXPECT_DEATH({ XPP_ASSERT(x > 0, "x must be positive"); }, "x > 0");
}

TEST(PanicDeathTest, AssertFormatsArguments) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  std::size_t idx = 7, size = 4;
  EXPECT_DEATH(
    { XPP_ASSERT(idx < size, "idx=%zu size=%zu", idx, size); },
    "assertion failed: idx < size .* idx=7 size=4");
}

/* ── XPP_DEBUG_ASSERT ── */

#ifndef NDEBUG
TEST(PanicDeathTest, DebugAssertAbortsInDebugBuild) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH({ XPP_DEBUG_ASSERT(false, "debug check"); }, "debug check");
}
#else
TEST(PanicTest, DebugAssertCompiledOutInReleaseBuild) {
  // With NDEBUG defined, XPP_DEBUG_ASSERT(false, ...) must be a no-op.
  XPP_DEBUG_ASSERT(false, "this must not abort");
  SUCCEED();
}
#endif

/* ── Integration with xLog callback ── */

namespace {
struct CapturedPanic {
  std::string msg;
  bool        had_backtrace = false;
  int         count        = 0;
};

void capture_panic(const char *msg, const char *backtrace, void *ud) {
  auto *cap         = static_cast<CapturedPanic *>(ud);
  cap->msg          = msg ? msg : "";
  cap->had_backtrace = (backtrace != nullptr);
  cap->count++;
  // Don't return — the contract says fatal callbacks must abort. Calling
  // std::abort() here is harmless; xLog will call it after we return too.
  std::abort();
}
} // namespace

TEST(PanicDeathTest, RoutesThroughXLogCallback) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
    {
      static CapturedPanic cap;
      xLogSetCallback(capture_panic, &cap);
      XPP_PANIC("routed");
    },
    "");
}

/* ── Variant panic paths ── */

TEST(PanicDeathTest, VariantGetWrongType) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Variant<int, double> v(3.14);
                 (void)v.get<int>();
               }()),
               "get<T>\\(\\) on Variant holding a different type");
}

TEST(PanicTest, VariantGetCorrectType) {
  xpp::Variant<int, double> v(42);
  EXPECT_EQ(v.get<int>(), 42);
}

TEST(PanicDeathTest, VariantGetWrongIndex) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Variant<int, double> v(xpp::InPlaceIndex<1>{}, 3.14);
                 (void)v.get<0>();
               }()),
               "get<N>\\(\\) on Variant holding a different alternative");
}
