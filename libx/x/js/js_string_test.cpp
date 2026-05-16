/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_string_test.cpp - xJSString creation / conversion / comparison.
 */

#include "js.h"
#include "js_private.h"

#include <gtest/gtest.h>

#include <cstring>

/* ─────────── Creation & accessors ─────────── */

TEST(XjsString, CreateFromChars) {
  const uint16_t chars[] = {'h', 'i'};
  xJSStringRef   s       = xJSStringCreateWithCharacters(chars, 2);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xJSStringGetLength(s), 2u);
  const uint16_t *p = xJSStringGetCharactersPtr(s);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p[0], 'h');
  EXPECT_EQ(p[1], 'i');
  EXPECT_EQ(p[2], 0); /* NUL-terminated */
  xJSStringRelease(s);
}

TEST(XjsString, CreateFromEmptyChars) {
  xJSStringRef s = xJSStringCreateWithCharacters(nullptr, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xJSStringGetLength(s), 0u);
  EXPECT_EQ(xJSStringGetCharactersPtr(s)[0], 0);
  xJSStringRelease(s);
}

TEST(XjsString, CreateFromCStringAscii) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("hello");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xJSStringGetLength(s), 5u);
  xJSStringRelease(s);
}

TEST(XjsString, CreateFromCStringNull) {
  /* NULL UTF-8 pointer produces an empty string (len=0). */
  xJSStringRef s = xJSStringCreateWithUTF8CString(nullptr);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xJSStringGetLength(s), 0u);
  xJSStringRelease(s);
}

TEST(XjsString, CreateFromCStringUnicode) {
  /* é (1 UTF-16 cu) + 中 (1) + 😀 (surrogate pair = 2) */
  xJSStringRef s = xJSStringCreateWithUTF8CString("é中😀");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xJSStringGetLength(s), 4u);
  xJSStringRelease(s);
}

/* ─────────── Retain / Release ─────────── */

TEST(XjsString, RetainRelease) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("abc");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->refcount, 1);
  xJSStringRef s2 = xJSStringRetain(s);
  EXPECT_EQ(s2, s);
  EXPECT_EQ(s->refcount, 2);
  xJSStringRelease(s);
  EXPECT_EQ(s->refcount, 1);
  xJSStringRelease(s); /* frees */
}

TEST(XjsString, NullSafeLifecycle) {
  EXPECT_EQ(xJSStringRetain(nullptr), nullptr);
  xJSStringRelease(nullptr);
  EXPECT_EQ(xJSStringGetLength(nullptr), 0u);
  EXPECT_EQ(xJSStringGetCharactersPtr(nullptr), nullptr);
  EXPECT_EQ(xJSStringGetMaximumUTF8CStringSize(nullptr), 1u);
}

/* ─────────── UTF-8 round trip ─────────── */

TEST(XjsString, GetUTF8CStringAscii) {
  xJSStringRef s       = xJSStringCreateWithUTF8CString("hello");
  char         buf[32] = {0};
  /* JSC returns size INCLUDING the NUL. */
  EXPECT_EQ(xJSStringGetUTF8CString(s, buf, sizeof(buf)), 6u);
  EXPECT_STREQ(buf, "hello");
  xJSStringRelease(s);
}

TEST(XjsString, GetUTF8CStringUnicode) {
  xJSStringRef s       = xJSStringCreateWithUTF8CString("中");
  char         buf[32] = {0};
  size_t       n       = xJSStringGetUTF8CString(s, buf, sizeof(buf));
  EXPECT_EQ(n, 4u); /* 3 UTF-8 bytes + NUL */
  EXPECT_STREQ(buf, "中");
  xJSStringRelease(s);
}

TEST(XjsString, GetUTF8CStringTruncatesToBuffer) {
  xJSStringRef s      = xJSStringCreateWithUTF8CString("hello");
  char         buf[4] = {'x', 'x', 'x', 'x'};
  size_t       n      = xJSStringGetUTF8CString(s, buf, 4);
  /* Must not overrun; last byte must be NUL. */
  EXPECT_EQ(buf[3], 0);
  EXPECT_LE(n, 4u);
  xJSStringRelease(s);
}

TEST(XjsString, GetUTF8CStringZeroBuffer) {
  xJSStringRef s      = xJSStringCreateWithUTF8CString("hi");
  char         buf[1] = {'Z'};
  EXPECT_EQ(xJSStringGetUTF8CString(s, buf, 0), 0u);
  EXPECT_EQ(buf[0], 'Z'); /* untouched */
  xJSStringRelease(s);
}

TEST(XjsString, GetUTF8CStringNullString) {
  char buf[4] = {'x', 'x', 'x', 'x'};
  EXPECT_EQ(xJSStringGetUTF8CString(nullptr, buf, 4), 0u);
  EXPECT_EQ(buf[0], 0);
}

TEST(XjsString, MaximumUTF8CStringSize) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("abcd");
  /* 4 UTF-16 code units -> 4*3 + 1 = 13 */
  EXPECT_EQ(xJSStringGetMaximumUTF8CStringSize(s), 13u);
  xJSStringRelease(s);
}

/* ─────────── Equality ─────────── */

TEST(XjsString, EqualSelf) {
  xJSStringRef s = xJSStringCreateWithUTF8CString("x");
  EXPECT_TRUE(xJSStringIsEqual(s, s));
  xJSStringRelease(s);
}

TEST(XjsString, EqualSameContent) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("abc");
  xJSStringRef b = xJSStringCreateWithUTF8CString("abc");
  EXPECT_TRUE(xJSStringIsEqual(a, b));
  xJSStringRelease(a);
  xJSStringRelease(b);
}

TEST(XjsString, NotEqualDifferentLen) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("abc");
  xJSStringRef b = xJSStringCreateWithUTF8CString("abcd");
  EXPECT_FALSE(xJSStringIsEqual(a, b));
  xJSStringRelease(a);
  xJSStringRelease(b);
}

TEST(XjsString, NotEqualSameLen) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("abc");
  xJSStringRef b = xJSStringCreateWithUTF8CString("abd");
  EXPECT_FALSE(xJSStringIsEqual(a, b));
  xJSStringRelease(a);
  xJSStringRelease(b);
}

TEST(XjsString, EqualNullHandling) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("x");
  EXPECT_FALSE(xJSStringIsEqual(a, nullptr));
  EXPECT_FALSE(xJSStringIsEqual(nullptr, a));
  /* Both NULL is treated as equal (a==b short-circuit). */
  EXPECT_TRUE(xJSStringIsEqual(nullptr, nullptr));
  xJSStringRelease(a);
}

TEST(XjsString, EqualToUTF8CString) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("hello");
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(a, "hello"));
  EXPECT_FALSE(xJSStringIsEqualToUTF8CString(a, "world"));
  EXPECT_FALSE(xJSStringIsEqualToUTF8CString(nullptr, "hello"));
  EXPECT_FALSE(xJSStringIsEqualToUTF8CString(a, nullptr));
  xJSStringRelease(a);
}

TEST(XjsString, EqualToUTF8CStringUnicode) {
  xJSStringRef a = xJSStringCreateWithUTF8CString("中😀");
  EXPECT_TRUE(xJSStringIsEqualToUTF8CString(a, "中😀"));
  EXPECT_FALSE(xJSStringIsEqualToUTF8CString(a, "中"));
  xJSStringRelease(a);
}
