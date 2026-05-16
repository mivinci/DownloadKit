/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 */

#include <x/base/string.h>

#include <gtest/gtest.h>
#include <string>

/* ───────────────────── Lifecycle ───────────────────── */

TEST(Str, CreateFromCStr) {
  xString s = xStringCreate("hello");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 5u);
  EXPECT_STREQ(s, "hello");
  xStringDestroy(s);
}

TEST(Str, CreateNull) {
  xString s = xStringCreate(NULL);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 0u);
  EXPECT_STREQ(s, "");
  xStringDestroy(s);
}

TEST(Str, CreateLen) {
  const char *data = "hello\0world";
  xString     s    = xStringCreateLen(data, 11);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 11u);
  EXPECT_EQ(memcmp(s, data, 11), 0);
  /* NUL-terminated at position 11 */
  EXPECT_EQ(s[11], '\0');
  xStringDestroy(s);
}

TEST(Str, CreateLenNull) {
  xString s = xStringCreateLen(NULL, 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 5u);
  xStringDestroy(s);
}

TEST(Str, DestroyNull) {
  xStringDestroy(NULL);
}

TEST(Str, Dup) {
  xString s  = xStringCreate("copy me");
  xString s2 = xStringDup(s);
  ASSERT_NE(s2, nullptr);
  EXPECT_STREQ(s2, "copy me");
  EXPECT_NE(s, s2); /* different pointers */
  xStringDestroy(s);
  xStringDestroy(s2);
}

TEST(Str, DupNull) {
  EXPECT_EQ(xStringDup(NULL), nullptr);
}

/* ───────────────────── Append ───────────────────── */

TEST(Str, Append) {
  xString s = xStringCreate("hello");
  xStringAppend(&s, " world");
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "hello world");
  EXPECT_EQ(xStringLen(s), 11u);
  xStringDestroy(s);
}

TEST(Str, AppendLen) {
  xString s = xStringCreate("hello");
  xStringAppendLen(&s, "\0world", 6);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 11u);
  xStringDestroy(s);
}

TEST(Str, AppendGrowth) {
  xString s = xStringCreate("");
  /* Append a large string to trigger growth */
  std::string big(4096, 'x');
  xStringAppend(&s, big.c_str());
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), big.size());
  EXPECT_EQ(s[big.size()], '\0');
  xStringDestroy(s);
}

TEST(Str, AppendFormat) {
  xString s = xStringCreate("answer: ");
  xStringAppendFormat(&s, "%d + %d = %d", 1, 2, 3);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "answer: 1 + 2 = 3");
  xStringDestroy(s);
}

TEST(Str, AppendFormatGrowth) {
  xString s = xStringCreate("");
  /* Format a string larger than initial capacity */
  xStringAppendFormat(&s, "%0400d", 42);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringLen(s), 400u);
  xStringDestroy(s);
}

/* ───────────────────── Truncate / Clear ───────────────────── */

TEST(Str, Truncate) {
  xString s = xStringCreate("hello world");
  xStringTruncate(s, 5);
  EXPECT_EQ(xStringLen(s), 5u);
  EXPECT_STREQ(s, "hello");
  /* Capacity should not shrink */
  EXPECT_GE(xStringCap(s), 11u);
  xStringDestroy(s);
}

TEST(Str, Clear) {
  xString s = xStringCreate("hello");
  xStringClear(s);
  EXPECT_EQ(xStringLen(s), 0u);
  EXPECT_STREQ(s, "");
  EXPECT_GE(xStringCap(s), 5u);
  xStringDestroy(s);
}

/* ───────────────────── Accessors ───────────────────── */

TEST(Str, LenNull) {
  EXPECT_EQ(xStringLen(NULL), 0u);
}

TEST(Str, CapNull) {
  EXPECT_EQ(xStringCap(NULL), 0u);
}

TEST(Str, AvailNull) {
  EXPECT_EQ(xStringAvail(NULL), 0u);
}

TEST(Str, Avail) {
  xString s = xStringCreate("hi");
  /* cap >= XSTRING_MIN_CAP (64), len == 2 */
  EXPECT_EQ(xStringAvail(s), xStringCap(s) - xStringLen(s));
  xStringDestroy(s);
}

/* ───────────────────── Memory control ───────────────────── */

TEST(Str, Grow) {
  xString s       = xStringCreate("small");
  size_t  old_cap = xStringCap(s);
  s               = xStringGrow(s, 1000);
  ASSERT_NE(s, nullptr);
  EXPECT_GE(xStringCap(s), xStringLen(s) + 1000);
  EXPECT_GT(xStringCap(s), old_cap);
  EXPECT_STREQ(s, "small");
  xStringDestroy(s);
}

TEST(Str, ShrinkToFit) {
  xString s = xStringCreate("");
  xStringAppend(&s, std::string(100, 'a').c_str());
  ASSERT_NE(s, nullptr);
  EXPECT_GT(xStringCap(s), 100u);

  s = xStringShrinkToFit(s);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStringCap(s), 100u);
  EXPECT_EQ(xStringLen(s), 100u);
  xStringDestroy(s);
}

/* ───────────────────── Search ───────────────────── */

TEST(Str, FindShort) {
  xString s = xStringCreate("hello world");
  EXPECT_EQ(xStringFindStr(s, "world"), 6u);
  EXPECT_EQ(xStringFindStr(s, "hello"), 0u);
  EXPECT_EQ(xStringFindStr(s, "x"), XSTRING_NONE);
  EXPECT_EQ(xStringFindStr(s, ""), 0u);
  xStringDestroy(s);
}

TEST(Str, FindBinarySafe) {
  /* "abc\0def" — strstr would stop at the embedded NUL */
  char    data[] = {'a', 'b', 'c', '\0', 'd', 'e', 'f'};
  xString s      = xStringCreateLen(data, 7);
  EXPECT_EQ(xStringFind(s, "def", 3), 4u);
  EXPECT_EQ(xStringFind(s, "ef", 2), 5u);
  EXPECT_EQ(xStringFind(s, "xyz", 3), XSTRING_NONE);
  xStringDestroy(s);
}

TEST(Str, FindEmptyNeedle) {
  xString s = xStringCreate("hello");
  EXPECT_EQ(xStringFind(s, "", 0), 0u);
  xStringDestroy(s);
}

TEST(Str, FindNeedleLongerThanHaystack) {
  xString s = xStringCreate("hi");
  EXPECT_EQ(xStringFind(s, "hello", 5), XSTRING_NONE);
  xStringDestroy(s);
}

TEST(Str, FindNull) {
  EXPECT_EQ(xStringFind(NULL, "x", 1), XSTRING_NONE);
  EXPECT_EQ(xStringFindStr(NULL, "x"), XSTRING_NONE);
  xString s = xStringCreate("hi");
  EXPECT_EQ(xStringFindStr(s, NULL), XSTRING_NONE);
  xStringDestroy(s);
}

TEST(Str, FindLongPattern) {
  /* Pattern >= XSTRING_FIND_THRESHOLD (32) triggers memmem path */
  std::string hay(200, 'a');
  hay += "TARGET";
  hay += std::string(100, 'b');

  xString s = xStringCreateLen(hay.data(), hay.size());
  EXPECT_EQ(xStringFindStr(s, "TARGET"), 200u);
  EXPECT_EQ(xStringFind(s, "TARGET", 6), 200u);
  xStringDestroy(s);
}

/* ───────────────────── Comparison ───────────────────── */

TEST(Str, Cmp) {
  xString a = xStringCreate("abc");
  xString b = xStringCreate("abd");
  xString c = xStringCreate("abc");

  EXPECT_LT(xStringCmp(a, b), 0);
  EXPECT_GT(xStringCmp(b, a), 0);
  EXPECT_EQ(xStringCmp(a, c), 0);

  xStringDestroy(a);
  xStringDestroy(b);
  xStringDestroy(c);
}

TEST(Str, CmpNull) {
  xString s = xStringCreate("x");
  EXPECT_LT(xStringCmp(NULL, s), 0);
  EXPECT_GT(xStringCmp(s, NULL), 0);
  EXPECT_EQ(xStringCmp(NULL, NULL), 0);
  xStringDestroy(s);
}

TEST(Str, Eq) {
  xString a = xStringCreate("hello");
  xString b = xStringCreate("hello");
  xString c = xStringCreate("world");

  EXPECT_TRUE(xStringEq(a, b));
  EXPECT_FALSE(xStringEq(a, c));
  EXPECT_TRUE(xStringEq(NULL, NULL));
  EXPECT_FALSE(xStringEq(a, NULL));

  xStringDestroy(a);
  xStringDestroy(b);
  xStringDestroy(c);
}

/* ───────────────────── C string compatibility ───────────────────── */

TEST(Str, CStrCompat) {
  xString s = xStringCreate("test string");
  /* Can be used with all standard C string functions */
  EXPECT_EQ(strlen(s), xStringLen(s));
  EXPECT_STREQ(s, "test string");
  EXPECT_NE(strstr(s, "str"), nullptr);

  xStringAppend(&s, " more");
  EXPECT_EQ(strlen(s), xStringLen(s));
  xStringDestroy(s);
}

/* ───────────────────── Binary safety ───────────────────── */

TEST(Str, BinarySafety) {
  char    data[] = {'a', '\0', 'b', '\0', 'c'};
  xString s      = xStringCreateLen(data, 5);
  EXPECT_EQ(xStringLen(s), 5u);
  EXPECT_EQ(memcmp(s, data, 5), 0);
  /* Still NUL-terminated at position 5 */
  EXPECT_EQ(s[5], '\0');
  xStringDestroy(s);
}
