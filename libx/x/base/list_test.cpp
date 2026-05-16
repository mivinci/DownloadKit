/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * list_test.cpp - xList unit tests
 */

#include <gtest/gtest.h>

#include <vector>

extern "C" {
#include <x/base/list.h>
}

/* ── Test element ── */

struct Item {
  int   value;
  xList node;
};

static Item *item_of(xList *n) {
  return xContainerOf(n, Item, node);
}

/* ── Fixture ── */

class ListTest : public ::testing::Test {
protected:
  xList head;

  void SetUp() override { xListInit(&head); }
};

/* ========== Init & Empty ========== */

TEST_F(ListTest, InitProducesEmptyList) {
  EXPECT_TRUE(xListEmpty(&head));
  EXPECT_EQ(head.next, &head);
  EXPECT_EQ(head.prev, &head);
}

TEST_F(ListTest, EmptyListForEachYieldsNothing) {
  int count = 0;
  xList *pos;
  xListForEach(pos, &head) { ++count; }
  EXPECT_EQ(count, 0);
}

TEST_F(ListTest, EmptyListForEachEntryYieldsNothing) {
  int count = 0;
  Item *pos;
  xListForEachEntry(pos, &head, node) { ++count; }
  EXPECT_EQ(count, 0);
}

/* ========== Add / AddHead / AddTail ========== */

TEST_F(ListTest, AddSingle) {
  Item a = {1, {}};
  xListAdd(&head, &a.node);

  EXPECT_FALSE(xListEmpty(&head));
  EXPECT_EQ(head.next, &a.node);
  EXPECT_EQ(head.prev, &a.node);
  EXPECT_EQ(a.node.next, &head);
  EXPECT_EQ(a.node.prev, &head);
}

TEST_F(ListTest, AddHeadOrdering) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddHead(&head, &a.node);
  xListAddHead(&head, &b.node);
  xListAddHead(&head, &c.node);

  /* c → b → a → head (stack order) */
  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], 3);
  EXPECT_EQ(vals[1], 2);
  EXPECT_EQ(vals[2], 1);
}

TEST_F(ListTest, AddTailOrdering) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  xListAddTail(&head, &c.node);

  /* a → b → c → head (queue order) */
  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 2);
  EXPECT_EQ(vals[2], 3);
}

TEST_F(ListTest, AddBefore) {
  Item a = {1, {}};
  Item b = {3, {}};
  Item c = {2, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  /* insert c before b (between a and b) */
  xListAddBefore(&b.node, &c.node);

  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 2);
  EXPECT_EQ(vals[2], 3);
}

/* ========== Del ========== */

TEST_F(ListTest, DelOnlyNode) {
  Item a = {1, {}};
  xListAddTail(&head, &a.node);
  EXPECT_FALSE(xListEmpty(&head));

  xListDel(&a.node);
  EXPECT_TRUE(xListEmpty(&head));
  EXPECT_EQ(head.next, &head);
  EXPECT_EQ(head.prev, &head);
}

TEST_F(ListTest, DelMiddleNode) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  xListAddTail(&head, &c.node);

  xListDel(&b.node);

  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 2u);
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 3);

  /* a → c links are correct */
  EXPECT_EQ(a.node.next, &c.node);
  EXPECT_EQ(c.node.prev, &a.node);
}

TEST_F(ListTest, DelFirstNode) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  xListAddTail(&head, &c.node);

  xListDel(&a.node);

  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 2u);
  EXPECT_EQ(vals[0], 2);
  EXPECT_EQ(vals[1], 3);
}

TEST_F(ListTest, DelLastNode) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  xListAddTail(&head, &c.node);

  xListDel(&c.node);

  std::vector<int> vals;
  Item *pos;
  xListForEachEntry(pos, &head, node) { vals.push_back(pos->value); }
  ASSERT_EQ(vals.size(), 2u);
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 2);
}

TEST_F(ListTest, DelPoisonsPointers) {
  Item a = {1, {}};
  xListAddTail(&head, &a.node);
  xListDel(&a.node);

  EXPECT_EQ(a.node.next, (xList *)0xDEAD);
  EXPECT_EQ(a.node.prev, (xList *)0xBEEF);
}

/* ========== ForEachSafe (delete during iteration) ========== */

TEST_F(ListTest, ForEachSafeDeleteAll) {
  Item items[5];
  for (int i = 0; i < 5; i++) {
    items[i].value = i + 1;
    xListAddTail(&head, &items[i].node);
  }

  xList *pos, *tmp;
  int count = 0;
  xListForEachSafe(pos, tmp, &head) {
    xListDel(pos);
    ++count;
  }
  EXPECT_EQ(count, 5);
  EXPECT_TRUE(xListEmpty(&head));
}

TEST_F(ListTest, ForEachEntrySafeDeleteOdd) {
  Item items[6];
  for (int i = 0; i < 6; i++) {
    items[i].value = i + 1;
    xListAddTail(&head, &items[i].node);
  }

  Item *pos, *tmp;
  xListForEachEntrySafe(pos, tmp, &head, node) {
    if (pos->value % 2 != 0) xListDel(&pos->node);
  }

  std::vector<int> vals;
  Item *p;
  xListForEachEntry(p, &head, node) { vals.push_back(p->value); }
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], 2);
  EXPECT_EQ(vals[1], 4);
  EXPECT_EQ(vals[2], 6);
}

/* ========== Circular integrity ========== */

TEST_F(ListTest, CircularIntegrity) {
  Item a = {1, {}};
  Item b = {2, {}};
  Item c = {3, {}};

  xListAddTail(&head, &a.node);
  xListAddTail(&head, &b.node);
  xListAddTail(&head, &c.node);

  /* head → a → b → c → head */
  EXPECT_EQ(head.next, &a.node);
  EXPECT_EQ(a.node.next, &b.node);
  EXPECT_EQ(b.node.next, &c.node);
  EXPECT_EQ(c.node.next, &head);

  /* head ← a ← b ← c ← head */
  EXPECT_EQ(head.prev, &c.node);
  EXPECT_EQ(c.node.prev, &b.node);
  EXPECT_EQ(b.node.prev, &a.node);
  EXPECT_EQ(a.node.prev, &head);
}

/* ========== xContainerOf integration ========== */

TEST_F(ListTest, ContainerOfRoundTrip) {
  Item a = {42, {}};
  xListAddTail(&head, &a.node);

  xList *first = head.next;
  Item *item = item_of(first);
  EXPECT_EQ(item, &a);
  EXPECT_EQ(item->value, 42);
}

/* ========== Scale & stress ========== */

TEST_F(ListTest, ManyItemsAddTailDelAll) {
  constexpr int N = 1000;
  std::vector<Item> items(N);

  for (int i = 0; i < N; i++) {
    items[i].value = i;
    xListAddTail(&head, &items[i].node);
  }
  EXPECT_FALSE(xListEmpty(&head));

  /* Delete in forward order */
  for (int i = 0; i < N; i++) {
    xListDel(&items[i].node);
  }
  EXPECT_TRUE(xListEmpty(&head));
}

TEST_F(ListTest, ManyItemsAddHeadDelReverse) {
  constexpr int N = 1000;
  std::vector<Item> items(N);

  for (int i = 0; i < N; i++) {
    items[i].value = i;
    xListAddHead(&head, &items[i].node);
  }

  /* Delete in reverse order */
  for (int i = N - 1; i >= 0; i--) {
    xListDel(&items[i].node);
  }
  EXPECT_TRUE(xListEmpty(&head));
}
