/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_test.cpp - Unit tests for xMap (hash, flat & tree backends)
 */

#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <unordered_set>

extern "C" {
#include <xbase/map.h>
}

/* ═══════════════════════════════════════════════════════════════════
 *  Parameterised test fixture — runs every test for both backends
 * ═══════════════════════════════════════════════════════════════════ */

class MapTest : public ::testing::TestWithParam<xMapType> {
protected:
  xMap m = nullptr;

  void SetUp() override {
    m = xMapCreate(GetParam(), 0, xMapStrHash, xMapStrEq);
    ASSERT_NE(m, nullptr);
  }

  void TearDown() override {
    if (m) xMapDestroy(m);
  }
};

/* ── Basic set / get / len ─────────────────────────────── */

TEST_P(MapTest, SetGetBasic) {
  int v1 = 10, v2 = 20;
  EXPECT_EQ(xMapSet(m, "hello", &v1), xErrno_Ok);
  EXPECT_EQ(xMapSet(m, "world", &v2), xErrno_Ok);
  EXPECT_EQ(xMapLen(m), 2u);

  EXPECT_EQ(xMapGet(m, "hello"), &v1);
  EXPECT_EQ(xMapGet(m, "world"), &v2);
  EXPECT_EQ(xMapGet(m, "missing"), nullptr);
}

/* ── Key update (overwrite) ────────────────────────────── */

TEST_P(MapTest, SetOverwrite) {
  int v1 = 1, v2 = 2;
  EXPECT_EQ(xMapSet(m, "key", &v1), xErrno_Ok);
  EXPECT_EQ(xMapGet(m, "key"), &v1);

  EXPECT_EQ(xMapSet(m, "key", &v2), xErrno_Ok);
  EXPECT_EQ(xMapGet(m, "key"), &v2);
  EXPECT_EQ(xMapLen(m), 1u); /* size unchanged */
}

/* ── Delete ────────────────────────────────────────────── */

TEST_P(MapTest, Delete) {
  int v = 42;
  EXPECT_EQ(xMapSet(m, "del_me", &v), xErrno_Ok);
  EXPECT_EQ(xMapLen(m), 1u);

  void *removed = xMapDel(m, "del_me");
  EXPECT_EQ(removed, &v);
  EXPECT_EQ(xMapLen(m), 0u);
  EXPECT_EQ(xMapGet(m, "del_me"), nullptr);
}

TEST_P(MapTest, DeleteNonExistent) {
  EXPECT_EQ(xMapDel(m, "nope"), nullptr);
}

/* ── Iterate ───────────────────────────────────────────── */

TEST_P(MapTest, IterateAll) {
  int vals[3] = {1, 2, 3};
  xMapSet(m, "a", &vals[0]);
  xMapSet(m, "b", &vals[1]);
  xMapSet(m, "c", &vals[2]);

  std::unordered_set<std::string> visited;
  xMapIterate(
    m,
    [](const void *key, void *val, void *arg) -> bool {
      auto *set = (std::unordered_set<std::string> *)arg;
      set->insert((const char *)key);
      (void)val;
      return true;
    },
    &visited);

  EXPECT_EQ(visited.size(), 3u);
  EXPECT_TRUE(visited.count("a"));
  EXPECT_TRUE(visited.count("b"));
  EXPECT_TRUE(visited.count("c"));
}

TEST_P(MapTest, IterateEarlyExit) {
  int vals[5] = {0, 1, 2, 3, 4};
  xMapSet(m, "a", &vals[0]);
  xMapSet(m, "b", &vals[1]);
  xMapSet(m, "c", &vals[2]);
  xMapSet(m, "d", &vals[3]);
  xMapSet(m, "e", &vals[4]);

  int count = 0;
  xMapIterate(
    m,
    [](const void *key, void *val, void *arg) -> bool {
      (void)key;
      (void)val;
      int *c = (int *)arg;
      (*c)++;
      return *c < 2; /* stop after 2 */
    },
    &count);

  EXPECT_EQ(count, 2);
}

/* ── Rehash / scale ────────────────────────────────────── */

TEST_P(MapTest, ManyInserts) {
  /* Use integer keys cast to (void *) for this test */
  xMap im = xMapCreate(GetParam(), 4, xMapIntHash, xMapIntEq);
  ASSERT_NE(im, nullptr);

  const int N = 1000;
  for (int i = 1; i <= N; i++) {
    void *key = (void *)(uintptr_t)i;
    void *val = (void *)(uintptr_t)(i * 10);
    EXPECT_EQ(xMapSet(im, key, val), xErrno_Ok);
  }
  EXPECT_EQ(xMapLen(im), (size_t)N);

  /* Verify all entries are retrievable */
  for (int i = 1; i <= N; i++) {
    void *key = (void *)(uintptr_t)i;
    void *val = xMapGet(im, key);
    EXPECT_EQ((uintptr_t)val, (uintptr_t)(i * 10));
  }

  /* Delete half and verify */
  for (int i = 1; i <= N / 2; i++) {
    void *key = (void *)(uintptr_t)i;
    void *val = xMapDel(im, key);
    EXPECT_EQ((uintptr_t)val, (uintptr_t)(i * 10));
  }
  EXPECT_EQ(xMapLen(im), (size_t)(N - N / 2));

  /* Remaining half still accessible */
  for (int i = N / 2 + 1; i <= N; i++) {
    void *key = (void *)(uintptr_t)i;
    void *val = xMapGet(im, key);
    EXPECT_EQ((uintptr_t)val, (uintptr_t)(i * 10));
  }

  xMapDestroy(im);
}

/* ── Empty map operations ──────────────────────────────── */

TEST_P(MapTest, EmptyMapOps) {
  EXPECT_EQ(xMapLen(m), 0u);
  EXPECT_EQ(xMapGet(m, "x"), nullptr);
  EXPECT_EQ(xMapDel(m, "x"), nullptr);

  int count = 0;
  xMapIterate(
    m,
    [](const void *key, void *val, void *arg) -> bool {
      (void)key;
      (void)val;
      (*(int *)arg)++;
      return true;
    },
    &count);
  EXPECT_EQ(count, 0);
}

/* ── NULL handle safety ────────────────────────────────── */

TEST_P(MapTest, NullHandleSafety) {
  EXPECT_EQ(xMapSet(nullptr, "k", nullptr), xErrno_InvalidArg);
  EXPECT_EQ(xMapGet(nullptr, "k"), nullptr);
  EXPECT_EQ(xMapDel(nullptr, "k"), nullptr);
  EXPECT_EQ(xMapLen(nullptr), 0u);
  xMapIterate(nullptr, nullptr, nullptr); /* should not crash */
  xMapDestroy(nullptr);                   /* should not crash */
}

/* ═══════════════════════════════════════════════════════════════════
 *  Instantiate for both backends
 * ═══════════════════════════════════════════════════════════════════ */

INSTANTIATE_TEST_SUITE_P(Hash, MapTest, ::testing::Values(xMapType_Hash),
                         [](const auto & /*info*/) { return "Hash"; });

INSTANTIATE_TEST_SUITE_P(Flat, MapTest, ::testing::Values(xMapType_Flat),
                         [](const auto & /*info*/) { return "Flat"; });

INSTANTIATE_TEST_SUITE_P(Tree, MapTest, ::testing::Values(xMapType_Tree),
                         [](const auto & /*info*/) { return "Tree"; });

/* ═══════════════════════════════════════════════════════════════════
 *  Non-parameterised tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(MapCreateTest, NullHashOrEq) {
  EXPECT_EQ(xMapCreate(xMapType_Hash, 0, nullptr, xMapStrEq), nullptr);
  EXPECT_EQ(xMapCreate(xMapType_Hash, 0, xMapStrHash, nullptr), nullptr);
  EXPECT_EQ(xMapCreate(xMapType_Flat, 0, nullptr, nullptr), nullptr);
}

TEST(MapCreateTest, TreeCreatesSuccessfully) {
  xMap m = xMapCreate(xMapType_Tree, 0, xMapStrHash, xMapStrEq);
  EXPECT_NE(m, nullptr);
  xMapDestroy(m);
}

TEST(MapCreateTest, InvalidTypeReturnsNull) {
  EXPECT_EQ(xMapCreate((xMapType)999, 0, xMapStrHash, xMapStrEq), nullptr);
}

/* ── Built-in hash / eq helpers ────────────────────────── */

TEST(MapHelpersTest, StrHash) {
  /* Same string → same hash */
  EXPECT_EQ(xMapStrHash("hello"), xMapStrHash("hello"));
  /* Different strings → (very likely) different hashes */
  EXPECT_NE(xMapStrHash("hello"), xMapStrHash("world"));
  /* Empty string is valid */
  uint64_t h = xMapStrHash("");
  (void)h;
}

TEST(MapHelpersTest, StrEq) {
  EXPECT_TRUE(xMapStrEq("abc", "abc"));
  EXPECT_FALSE(xMapStrEq("abc", "def"));
}

TEST(MapHelpersTest, IntHash) {
  EXPECT_EQ(xMapIntHash((void *)42), xMapIntHash((void *)42));
  EXPECT_NE(xMapIntHash((void *)1), xMapIntHash((void *)2));
  /* 0 is a valid key */
  uint64_t h = xMapIntHash((void *)0);
  (void)h;
}

TEST(MapHelpersTest, IntEq) {
  EXPECT_TRUE(xMapIntEq((void *)1, (void *)1));
  EXPECT_FALSE(xMapIntEq((void *)1, (void *)2));
}
