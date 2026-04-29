/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 */

#include <xbase/str.h>

#include <gtest/gtest.h>
#include <string>

/* ───────────────────── Lifecycle ───────────────────── */

TEST(Str, CreateFromCStr) {
  xStr s = xStrCreate("hello");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 5u);
  EXPECT_STREQ(s, "hello");
  xStrDestroy(s);
}

TEST(Str, CreateNull) {
  xStr s = xStrCreate(NULL);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 0u);
  EXPECT_STREQ(s, "");
  xStrDestroy(s);
}

TEST(Str, CreateLen) {
  const char *data = "hello\0world";
  xStr        s    = xStrCreateLen(data, 11);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 11u);
  EXPECT_EQ(memcmp(s, data, 11), 0);
  /* NUL-terminated at position 11 */
  EXPECT_EQ(s[11], '\0');
  xStrDestroy(s);
}

TEST(Str, CreateLenNull) {
  xStr s = xStrCreateLen(NULL, 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 5u);
  xStrDestroy(s);
}

TEST(Str, DestroyNull) {
  xStrDestroy(NULL);
}

TEST(Str, Dup) {
  xStr s  = xStrCreate("copy me");
  xStr s2 = xStrDup(s);
  ASSERT_NE(s2, nullptr);
  EXPECT_STREQ(s2, "copy me");
  EXPECT_NE(s, s2); /* different pointers */
  xStrDestroy(s);
  xStrDestroy(s2);
}

TEST(Str, DupNull) {
  EXPECT_EQ(xStrDup(NULL), nullptr);
}

/* ───────────────────── Append ───────────────────── */

TEST(Str, Append) {
  xStr s = xStrCreate("hello");
  s      = xStrAppend(s, " world");
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "hello world");
  EXPECT_EQ(xStrLen(s), 11u);
  xStrDestroy(s);
}

TEST(Str, AppendLen) {
  xStr s = xStrCreate("hello");
  s      = xStrAppendLen(s, "\0world", 6);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 11u);
  xStrDestroy(s);
}

TEST(Str, AppendGrowth) {
  xStr s = xStrCreate("");
  /* Append a large string to trigger growth */
  std::string big(4096, 'x');
  s = xStrAppend(s, big.c_str());
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), big.size());
  EXPECT_EQ(s[big.size()], '\0');
  xStrDestroy(s);
}

TEST(Str, AppendFormat) {
  xStr s = xStrCreate("answer: ");
  s      = xStrAppendFormat(s, "%d + %d = %d", 1, 2, 3);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "answer: 1 + 2 = 3");
  xStrDestroy(s);
}

TEST(Str, AppendFormatGrowth) {
  xStr s = xStrCreate("");
  /* Format a string larger than initial capacity */
  s = xStrAppendFormat(s, "%0400d", 42);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrLen(s), 400u);
  xStrDestroy(s);
}

/* ───────────────────── Truncate / Clear ───────────────────── */

TEST(Str, Truncate) {
  xStr s = xStrCreate("hello world");
  xStrTruncate(s, 5);
  EXPECT_EQ(xStrLen(s), 5u);
  EXPECT_STREQ(s, "hello");
  /* Capacity should not shrink */
  EXPECT_GE(xStrCap(s), 11u);
  xStrDestroy(s);
}

TEST(Str, Clear) {
  xStr s = xStrCreate("hello");
  xStrClear(s);
  EXPECT_EQ(xStrLen(s), 0u);
  EXPECT_STREQ(s, "");
  EXPECT_GE(xStrCap(s), 5u);
  xStrDestroy(s);
}

/* ───────────────────── Accessors ───────────────────── */

TEST(Str, LenNull) {
  EXPECT_EQ(xStrLen(NULL), 0u);
}

TEST(Str, CapNull) {
  EXPECT_EQ(xStrCap(NULL), 0u);
}

TEST(Str, AvailNull) {
  EXPECT_EQ(xStrAvail(NULL), 0u);
}

TEST(Str, Avail) {
  xStr s = xStrCreate("hi");
  /* cap >= XSTR_MIN_CAP (64), len == 2 */
  EXPECT_EQ(xStrAvail(s), xStrCap(s) - xStrLen(s));
  xStrDestroy(s);
}

/* ───────────────────── Memory control ───────────────────── */

TEST(Str, Grow) {
  xStr   s       = xStrCreate("small");
  size_t old_cap = xStrCap(s);
  s              = xStrGrow(s, 1000);
  ASSERT_NE(s, nullptr);
  EXPECT_GE(xStrCap(s), xStrLen(s) + 1000);
  EXPECT_GT(xStrCap(s), old_cap);
  EXPECT_STREQ(s, "small");
  xStrDestroy(s);
}

TEST(Str, ShrinkToFit) {
  xStr s = xStrCreate("");
  s      = xStrAppend(s, std::string(100, 'a').c_str());
  ASSERT_NE(s, nullptr);
  EXPECT_GT(xStrCap(s), 100u);

  s = xStrShrinkToFit(s);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(xStrCap(s), 100u);
  EXPECT_EQ(xStrLen(s), 100u);
  xStrDestroy(s);
}

/* ───────────────────── Search ───────────────────── */

TEST(Str, FindShort) {
  xStr s = xStrCreate("hello world");
  EXPECT_EQ(xStrFindStr(s, "world"), 6u);
  EXPECT_EQ(xStrFindStr(s, "hello"), 0u);
  EXPECT_EQ(xStrFindStr(s, "x"), XSTR_NONE);
  EXPECT_EQ(xStrFindStr(s, ""), 0u);
  xStrDestroy(s);
}

TEST(Str, FindBinarySafe) {
  /* "abc\0def" — strstr would stop at the embedded NUL */
  char data[] = {'a', 'b', 'c', '\0', 'd', 'e', 'f'};
  xStr s      = xStrCreateLen(data, 7);
  EXPECT_EQ(xStrFind(s, "def", 3), 4u);
  EXPECT_EQ(xStrFind(s, "ef", 2), 5u);
  EXPECT_EQ(xStrFind(s, "xyz", 3), XSTR_NONE);
  xStrDestroy(s);
}

TEST(Str, FindEmptyNeedle) {
  xStr s = xStrCreate("hello");
  EXPECT_EQ(xStrFind(s, "", 0), 0u);
  xStrDestroy(s);
}

TEST(Str, FindNeedleLongerThanHaystack) {
  xStr s = xStrCreate("hi");
  EXPECT_EQ(xStrFind(s, "hello", 5), XSTR_NONE);
  xStrDestroy(s);
}

TEST(Str, FindNull) {
  EXPECT_EQ(xStrFind(NULL, "x", 1), XSTR_NONE);
  EXPECT_EQ(xStrFindStr(NULL, "x"), XSTR_NONE);
  xStr s = xStrCreate("hi");
  EXPECT_EQ(xStrFindStr(s, NULL), XSTR_NONE);
  xStrDestroy(s);
}

TEST(Str, FindLongPattern) {
  /* Pattern >= XSTR_FIND_THRESHOLD (32) triggers memmem path */
  std::string hay(200, 'a');
  hay += "TARGET";
  hay += std::string(100, 'b');

  xStr s = xStrCreateLen(hay.data(), hay.size());
  EXPECT_EQ(xStrFindStr(s, "TARGET"), 200u);
  EXPECT_EQ(xStrFind(s, "TARGET", 6), 200u);
  xStrDestroy(s);
}

/* ───────────────────── Comparison ───────────────────── */

TEST(Str, Cmp) {
  xStr a = xStrCreate("abc");
  xStr b = xStrCreate("abd");
  xStr c = xStrCreate("abc");

  EXPECT_LT(xStrCmp(a, b), 0);
  EXPECT_GT(xStrCmp(b, a), 0);
  EXPECT_EQ(xStrCmp(a, c), 0);

  xStrDestroy(a);
  xStrDestroy(b);
  xStrDestroy(c);
}

TEST(Str, CmpNull) {
  xStr s = xStrCreate("x");
  EXPECT_LT(xStrCmp(NULL, s), 0);
  EXPECT_GT(xStrCmp(s, NULL), 0);
  EXPECT_EQ(xStrCmp(NULL, NULL), 0);
  xStrDestroy(s);
}

TEST(Str, Eq) {
  xStr a = xStrCreate("hello");
  xStr b = xStrCreate("hello");
  xStr c = xStrCreate("world");

  EXPECT_TRUE(xStrEq(a, b));
  EXPECT_FALSE(xStrEq(a, c));
  EXPECT_TRUE(xStrEq(NULL, NULL));
  EXPECT_FALSE(xStrEq(a, NULL));

  xStrDestroy(a);
  xStrDestroy(b);
  xStrDestroy(c);
}

/* ───────────────────── C string compatibility ───────────────────── */

TEST(Str, CStrCompat) {
  xStr s = xStrCreate("test string");
  /* Can be used with all standard C string functions */
  EXPECT_EQ(strlen(s), xStrLen(s));
  EXPECT_STREQ(s, "test string");
  EXPECT_NE(strstr(s, "str"), nullptr);

  s = xStrAppend(s, " more");
  EXPECT_EQ(strlen(s), xStrLen(s));
  xStrDestroy(s);
}

/* ───────────────────── Binary safety ───────────────────── */

TEST(Str, BinarySafety) {
  char data[] = {'a', '\0', 'b', '\0', 'c'};
  xStr s      = xStrCreateLen(data, 5);
  EXPECT_EQ(xStrLen(s), 5u);
  EXPECT_EQ(memcmp(s, data, 5), 0);
  /* Still NUL-terminated at position 5 */
  EXPECT_EQ(s[5], '\0');
  xStrDestroy(s);
}
