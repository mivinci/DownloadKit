/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_test.cpp - xMpsc 单元测试
 */

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/mpsc.h>
}

/* 测试用节点：内嵌 xMpsc 链表节点 + 一个标识值 */
struct TestNode {
  xMpsc mpsc;
  int   value;
};

/* 测试夹具：每个测试用例自动初始化 head/tail */
class MpscTest : public ::testing::Test {
protected:
  xMpsc *head = nullptr;
  xMpsc *tail = nullptr;

  void SetUp() override {
    head = nullptr;
    tail = nullptr;
  }

  /* 辅助：push 一个 TestNode */
  void Push(TestNode *node) {
    xMpscPush(&head, &tail, &node->mpsc);
  }

  /* 辅助：pop 并返回 TestNode，队列为空时返回 nullptr */
  TestNode *Pop() {
    xMpsc *n = xMpscPop(&head, &tail);
    if (!n) return nullptr;
    return reinterpret_cast<TestNode *>(reinterpret_cast<char *>(n) -
                                        offsetof(TestNode, mpsc));
  }
};

/* ========== 基本功能测试 ========== */

/* 空队列 pop 应返回 nullptr */
TEST_F(MpscTest, PopFromEmptyQueue) {
  EXPECT_EQ(Pop(), nullptr);
}

/* 空队列 xMpscEmpty 应返回 true */
TEST_F(MpscTest, EmptyQueueIsEmpty) {
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* push 一个节点后队列不为空 */
TEST_F(MpscTest, NonEmptyAfterPush) {
  TestNode node{.mpsc = {}, .value = 1};
  Push(&node);
  EXPECT_FALSE(xMpscEmpty(&head));
}

/* push 一个节点后 pop 应返回该节点 */
TEST_F(MpscTest, PushOneThenPop) {
  TestNode node{.mpsc = {}, .value = 42};
  Push(&node);

  TestNode *popped = Pop();
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->value, 42);

  /* 队列应为空 */
  EXPECT_EQ(Pop(), nullptr);
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* 多节点 push 后 pop 应保持 FIFO 顺序 */
TEST_F(MpscTest, FIFOOrder) {
  constexpr int N = 10;
  TestNode      nodes[N];
  for (int i = 0; i < N; i++) {
    nodes[i].value = i;
    Push(&nodes[i]);
  }

  for (int i = 0; i < N; i++) {
    TestNode *popped = Pop();
    ASSERT_NE(popped, nullptr) << "第 " << i << " 次 pop 不应为空";
    EXPECT_EQ(popped->value, i) << "第 " << i << " 次 pop 顺序错误";
  }

  EXPECT_EQ(Pop(), nullptr);
}

/* pop 至空后队列状态正确 */
TEST_F(MpscTest, PopUntilEmpty) {
  constexpr int N = 5;
  TestNode      nodes[N];
  for (int i = 0; i < N; i++) {
    nodes[i].value = i;
    Push(&nodes[i]);
  }

  int count = 0;
  while (Pop() != nullptr) {
    count++;
  }
  EXPECT_EQ(count, N);
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* 队列清空后可以重新使用 */
TEST_F(MpscTest, ReuseAfterDrain) {
  /* 第一轮 */
  TestNode n1{.mpsc = {}, .value = 1};
  TestNode n2{.mpsc = {}, .value = 2};
  Push(&n1);
  Push(&n2);
  EXPECT_EQ(Pop()->value, 1);
  EXPECT_EQ(Pop()->value, 2);
  EXPECT_EQ(Pop(), nullptr);
  EXPECT_TRUE(xMpscEmpty(&head));

  /* 第二轮：重新 push */
  TestNode n3{.mpsc = {}, .value = 3};
  TestNode n4{.mpsc = {}, .value = 4};
  Push(&n3);
  Push(&n4);
  EXPECT_FALSE(xMpscEmpty(&head));
  EXPECT_EQ(Pop()->value, 3);
  EXPECT_EQ(Pop()->value, 4);
  EXPECT_EQ(Pop(), nullptr);
}

/* 交替 push 和 pop */
TEST_F(MpscTest, InterleavedPushPop) {
  TestNode n1{.mpsc = {}, .value = 1};
  TestNode n2{.mpsc = {}, .value = 2};
  TestNode n3{.mpsc = {}, .value = 3};

  Push(&n1);
  EXPECT_EQ(Pop()->value, 1);
  EXPECT_EQ(Pop(), nullptr);

  Push(&n2);
  Push(&n3);
  EXPECT_EQ(Pop()->value, 2);
  EXPECT_EQ(Pop()->value, 3);
  EXPECT_EQ(Pop(), nullptr);
}

/* push 单个节点后 pop，再 push 单个节点后 pop（测试单节点 CAS 路径） */
TEST_F(MpscTest, SingleNodeCASPath) {
  TestNode n1{.mpsc = {}, .value = 10};
  Push(&n1);
  TestNode *p = Pop();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 10);
  EXPECT_TRUE(xMpscEmpty(&head));

  /* 再次 push 单个节点 */
  TestNode n2{.mpsc = {}, .value = 20};
  Push(&n2);
  p = Pop();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 20);
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* ========== 并发测试 ========== */

/* 多生产者单消费者：所有 push 的节点都能被 pop 出来 */
TEST_F(MpscTest, ConcurrentMultiProducerSingleConsumer) {
  constexpr int NUM_PRODUCERS      = 4;
  constexpr int NODES_PER_PRODUCER = 1000;
  constexpr int TOTAL_NODES        = NUM_PRODUCERS * NODES_PER_PRODUCER;

  /* 每个生产者拥有自己的节点数组 */
  std::vector<std::vector<TestNode>> producer_nodes(NUM_PRODUCERS);
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producer_nodes[p].resize(NODES_PER_PRODUCER);
    for (int i = 0; i < NODES_PER_PRODUCER; i++) {
      /* value 编码：高位为生产者 ID，低位为序号 */
      producer_nodes[p][i].value = p * NODES_PER_PRODUCER + i;
    }
  }

  std::atomic<bool> start{false};

  /* 启动生产者线程 */
  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p]() {
      while (!start.load(std::memory_order_acquire)) {
        /* spin 等待同时开始 */
      }
      for (int i = 0; i < NODES_PER_PRODUCER; i++) {
        xMpscPush(&head, &tail, &producer_nodes[p][i].mpsc);
      }
    });
  }

  /* 启动消费者线程 */
  std::vector<int> consumed;
  consumed.reserve(TOTAL_NODES);

  std::thread consumer([&]() {
    while (!start.load(std::memory_order_acquire)) {
      /* spin 等待同时开始 */
    }
    int empty_spins = 0;
    while (consumed.size() < static_cast<size_t>(TOTAL_NODES)) {
      xMpsc *n = xMpscPop(&head, &tail);
      if (n) {
        TestNode *tn = reinterpret_cast<TestNode *>(
          reinterpret_cast<char *>(n) - offsetof(TestNode, mpsc));
        consumed.push_back(tn->value);
        empty_spins = 0;
      } else {
        empty_spins++;
        if (empty_spins > 1000000) {
          /* 安全退出：防止死循环 */
          break;
        }
      }
    }
  });

  /* 同时启动所有线程 */
  start.store(true, std::memory_order_release);

  for (auto &t : producers)
    t.join();
  consumer.join();

  /* 验证：所有节点都被消费 */
  EXPECT_EQ(consumed.size(), static_cast<size_t>(TOTAL_NODES));

  /* 验证：没有重复、没有遗漏 */
  std::set<int> consumed_set(consumed.begin(), consumed.end());
  EXPECT_EQ(consumed_set.size(), static_cast<size_t>(TOTAL_NODES));
  for (int i = 0; i < TOTAL_NODES; i++) {
    EXPECT_TRUE(consumed_set.count(i)) << "缺少节点 value=" << i;
  }
}

/* 并发 push 后批量 pop：先并发 push 完成，再单线程 pop 全部 */
TEST_F(MpscTest, ConcurrentPushThenSequentialPop) {
  constexpr int NUM_PRODUCERS      = 8;
  constexpr int NODES_PER_PRODUCER = 500;
  constexpr int TOTAL_NODES        = NUM_PRODUCERS * NODES_PER_PRODUCER;

  std::vector<std::vector<TestNode>> producer_nodes(NUM_PRODUCERS);
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producer_nodes[p].resize(NODES_PER_PRODUCER);
    for (int i = 0; i < NODES_PER_PRODUCER; i++) {
      producer_nodes[p][i].value = p * NODES_PER_PRODUCER + i;
    }
  }

  std::atomic<bool>        start{false};
  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p]() {
      while (!start.load(std::memory_order_acquire)) {}
      for (int i = 0; i < NODES_PER_PRODUCER; i++) {
        xMpscPush(&head, &tail, &producer_nodes[p][i].mpsc);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto &t : producers)
    t.join();

  /* 所有生产者完成后，单线程 pop */
  std::set<int> consumed_set;
  int           count = 0;
  while (true) {
    xMpsc *n = xMpscPop(&head, &tail);
    if (!n) break;
    TestNode *tn = reinterpret_cast<TestNode *>(reinterpret_cast<char *>(n) -
                                                offsetof(TestNode, mpsc));
    consumed_set.insert(tn->value);
    count++;
  }

  EXPECT_EQ(count, TOTAL_NODES);
  EXPECT_EQ(consumed_set.size(), static_cast<size_t>(TOTAL_NODES));
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* 高并发压力测试：大量生产者 + 大量节点 */
TEST_F(MpscTest, StressTest) {
  constexpr int NUM_PRODUCERS      = 16;
  constexpr int NODES_PER_PRODUCER = 2000;
  constexpr int TOTAL_NODES        = NUM_PRODUCERS * NODES_PER_PRODUCER;

  std::vector<std::vector<TestNode>> producer_nodes(NUM_PRODUCERS);
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producer_nodes[p].resize(NODES_PER_PRODUCER);
    for (int i = 0; i < NODES_PER_PRODUCER; i++) {
      producer_nodes[p][i].value = p * NODES_PER_PRODUCER + i;
    }
  }

  std::atomic<bool> start{false};
  std::atomic<int>  total_consumed{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p]() {
      while (!start.load(std::memory_order_acquire)) {}
      for (int i = 0; i < NODES_PER_PRODUCER; i++) {
        xMpscPush(&head, &tail, &producer_nodes[p][i].mpsc);
      }
    });
  }

  /* 消费者线程 */
  std::thread consumer([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    int empty_spins = 0;
    while (total_consumed.load(std::memory_order_relaxed) < TOTAL_NODES) {
      xMpsc *n = xMpscPop(&head, &tail);
      if (n) {
        total_consumed.fetch_add(1, std::memory_order_relaxed);
        empty_spins = 0;
      } else {
        empty_spins++;
        if (empty_spins > 5000000) break;
      }
    }
  });

  start.store(true, std::memory_order_release);

  for (auto &t : producers)
    t.join();
  consumer.join();

  EXPECT_EQ(total_consumed.load(), TOTAL_NODES);
  EXPECT_TRUE(xMpscEmpty(&head));
}

/* 单生产者单消费者并发 */
TEST_F(MpscTest, SingleProducerSingleConsumer) {
  constexpr int N = 5000;

  std::vector<TestNode> nodes(N);
  for (int i = 0; i < N; i++) {
    nodes[i].value = i;
  }

  std::atomic<bool> start{false};
  std::atomic<bool> producer_done{false};

  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    for (int i = 0; i < N; i++) {
      xMpscPush(&head, &tail, &nodes[i].mpsc);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<int> consumed;
  consumed.reserve(N);

  std::thread consumer([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    while (true) {
      xMpsc *n = xMpscPop(&head, &tail);
      if (n) {
        TestNode *tn = reinterpret_cast<TestNode *>(
          reinterpret_cast<char *>(n) - offsetof(TestNode, mpsc));
        consumed.push_back(tn->value);
      } else if (producer_done.load(std::memory_order_acquire)) {
        /* producer is done, drain remaining nodes.
         * there may be a visibility window between the last push's
         * xAtomicXchg and the next-pointer update, so keep retrying. */
        int empty_spins = 0;
        while (empty_spins < 1000) {
          n = xMpscPop(&head, &tail);
          if (n) {
            TestNode *tn = reinterpret_cast<TestNode *>(
              reinterpret_cast<char *>(n) - offsetof(TestNode, mpsc));
            consumed.push_back(tn->value);
            empty_spins = 0;
          } else {
            empty_spins++;
          }
        }
        break;
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  /* 单生产者场景下，pop 顺序应与 push 顺序一致 */
  EXPECT_EQ(consumed.size(), static_cast<size_t>(N));
  for (size_t i = 0; i < consumed.size(); i++) {
    EXPECT_EQ(consumed[i], static_cast<int>(i))
      << "FIFO 顺序在第 " << i << " 个节点处不一致";
  }
}

/* 多生产者场景下，同一生产者内部的节点保持相对顺序 */
TEST_F(MpscTest, PerProducerFIFOOrder) {
  constexpr int NUM_PRODUCERS      = 4;
  constexpr int NODES_PER_PRODUCER = 500;

  std::vector<std::vector<TestNode>> producer_nodes(NUM_PRODUCERS);
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producer_nodes[p].resize(NODES_PER_PRODUCER);
    for (int i = 0; i < NODES_PER_PRODUCER; i++) {
      /* value 编码：高 16 位 = 生产者 ID，低 16 位 = 序号 */
      producer_nodes[p][i].value = (p << 16) | i;
    }
  }

  std::atomic<bool>        start{false};
  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p]() {
      while (!start.load(std::memory_order_acquire)) {}
      for (int i = 0; i < NODES_PER_PRODUCER; i++) {
        xMpscPush(&head, &tail, &producer_nodes[p][i].mpsc);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto &t : producers)
    t.join();

  /* 按生产者分组收集 pop 出来的序号 */
  std::vector<std::vector<int>> per_producer_order(NUM_PRODUCERS);
  while (true) {
    xMpsc *n = xMpscPop(&head, &tail);
    if (!n) break;
    TestNode *tn = reinterpret_cast<TestNode *>(reinterpret_cast<char *>(n) -
                                                offsetof(TestNode, mpsc));
    int       producer_id = (tn->value >> 16) & 0xFFFF;
    int       seq         = tn->value & 0xFFFF;
    per_producer_order[producer_id].push_back(seq);
  }

  /* 验证每个生产者内部的节点保持 FIFO 顺序 */
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    ASSERT_EQ(per_producer_order[p].size(),
              static_cast<size_t>(NODES_PER_PRODUCER))
      << "生产者 " << p << " 的节点数量不正确";
    for (int i = 0; i < NODES_PER_PRODUCER; i++) {
      EXPECT_EQ(per_producer_order[p][i], i)
        << "生产者 " << p << " 的第 " << i << " 个节点顺序错误";
    }
  }
}
