/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task_test.cpp - xTask unit tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <x/base/task.h>
}

/* ── Helpers ── */

struct Counter {
  std::atomic<int> value{0};
};

static void *increment(void *arg) {
  auto *c = static_cast<Counter *>(arg);
  c->value.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

static void *noop(void *) {
  return nullptr;
}

/* ── Fixture ── */

class TaskTest : public ::testing::Test {
protected:
  xTaskGroup g = nullptr;

  void SetUp() override {
    xTaskGroupConf conf = {};
    conf.nthreads       = 4;
    g                   = xTaskGroupCreate(&conf);
    ASSERT_NE(g, nullptr);
  }

  void TearDown() override {
    if (g) xTaskGroupDestroy(g);
  }
};

/* ========== Basic ========== */

TEST_F(TaskTest, CreateAndDestroy) {
  /* Should not crash */
}

TEST_F(TaskTest, ThreadsCount) {
  /* Lazy-loading: no thread created until first task */
  EXPECT_EQ(xTaskGroupThreads(g), (size_t)0);

  /* Submit one task — thread should be spawned */
  xTask t = xTaskSubmit(g, noop, nullptr);
  ASSERT_NE(t, nullptr);
  xTaskWait(t, nullptr);

  /* At least one thread should have been created */
  EXPECT_GE(xTaskGroupThreads(g), (size_t)1);
}

TEST_F(TaskTest, PendingInitiallyZero) {
  EXPECT_EQ(xTaskGroupPending(g), (size_t)0);
}

/* ========== Single Task ========== */

TEST_F(TaskTest, SubmitAndWaitSingle) {
  Counter c;
  xTask   t = xTaskSubmit(g, increment, &c);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);
  EXPECT_EQ(c.value.load(), 1);
}

TEST_F(TaskTest, WaitNullReturnsError) {
  EXPECT_EQ(xTaskWait(nullptr, nullptr), xErrno_InvalidArg);
}

TEST_F(TaskTest, SubmitNullFnReturnsNull) {
  EXPECT_EQ(xTaskSubmit(g, nullptr, nullptr), nullptr);
}

TEST_F(TaskTest, SubmitNullGroupReturnsNull) {
  EXPECT_EQ(xTaskSubmit(nullptr, noop, nullptr), nullptr);
}

/* ========== Multiple Tasks ========== */

TEST_F(TaskTest, SubmitManyAndWaitAll) {
  constexpr int      N = 1000;
  Counter            counter;
  std::vector<xTask> tasks(N);

  for (int i = 0; i < N; i++) {
    tasks[i] = xTaskSubmit(g, increment, &counter);
    ASSERT_NE(tasks[i], nullptr);
  }

  for (auto t : tasks) {
    EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);
  }

  EXPECT_EQ(counter.value.load(), N);
}

TEST_F(TaskTest, GroupWait) {
  constexpr int N = 500;
  Counter       counter;

  for (int i = 0; i < N; i++) {
    xTask t = xTaskSubmit(g, increment, &counter);
    ASSERT_NE(t, nullptr);
    (void)t; /* leak is fine — GroupWait guarantees completion */
  }

  EXPECT_EQ(xTaskGroupWait(g), xErrno_Ok);
  EXPECT_EQ(counter.value.load(), N);
  EXPECT_EQ(xTaskGroupPending(g), (size_t)0);
}

TEST_F(TaskTest, PendingCount) {
  /* Submit a blocking task first so subsequent submits queue up. */
  std::atomic<bool> unblock{false};

  auto block_fn = [](void *arg) -> void * {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    while (!flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  xTaskGroupConf conf   = {.nthreads = 1, .queue_cap = 0};
  xTaskGroup     single = xTaskGroupCreate(&conf);
  ASSERT_NE(single, nullptr);

  /* Fill the single worker with a blocking task */
  xTaskSubmit(single, block_fn, &unblock);

  /* Submit more tasks — they should queue up */
  for (int i = 0; i < 5; i++) {
    xTaskSubmit(single, noop, nullptr);
  }

  /* Pending should be 6 (1 running + 5 queued) */
  EXPECT_EQ(xTaskGroupPending(single), (size_t)6);

  /* Unblock the worker */
  unblock.store(true, std::memory_order_release);
  xTaskGroupWait(single);
  EXPECT_EQ(xTaskGroupPending(single), (size_t)0);

  xTaskGroupDestroy(single);
}

/* ========== Concurrent Submit ========== */

TEST_F(TaskTest, ConcurrentSubmits) {
  constexpr int            THREADS    = 8;
  constexpr int            PER_THREAD = 200;
  Counter                  counter;
  std::vector<std::thread> threads;

  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&]() {
      for (int i = 0; i < PER_THREAD; i++) {
        xTaskSubmit(g, increment, &counter);
      }
    });
  }

  for (auto &th : threads)
    th.join();
  xTaskGroupWait(g);

  EXPECT_EQ(counter.value.load(), THREADS * PER_THREAD);
}

/* ========== Queue Capacity ========== */

TEST_F(TaskTest, QueueCapRejectsWhenFull) {
  xTaskGroupConf conf  = {.nthreads = 1, .queue_cap = 2};
  xTaskGroup     small = xTaskGroupCreate(&conf);
  ASSERT_NE(small, nullptr);

  /* Block the single worker so tasks accumulate in the queue. */
  std::atomic<bool> unblock{false};
  auto              block_fn = [](void *arg) -> void              *{
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    while (!flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  /* Submit blocking task to occupy the worker */
  xTaskSubmit(small, block_fn, &unblock);

  /* Give the worker time to dequeue the blocking task.
   * After this, the queue should be empty (qsize=0, pending=1 running). */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Now fill the queue up to capacity */
  xTask t1 = xTaskSubmit(small, noop, nullptr);
  xTask t2 = xTaskSubmit(small, noop, nullptr);
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  /* Queue should be full — next submit returns NULL */
  xTask t3 = xTaskSubmit(small, noop, nullptr);
  EXPECT_EQ(t3, nullptr);

  /* Cleanup: unblock worker, then wait for all tasks to finish */
  unblock.store(true, std::memory_order_release);
  xTaskGroupWait(small);

  /* Now safe to wait on individual tasks */
  xTaskWait(t1, nullptr);
  xTaskWait(t2, nullptr);
  xTaskGroupDestroy(small);
}

/* ========== Auto Thread Count ========== */

TEST(TaskGroupAuto, ZeroThreadsAutoDetect) {
  xTaskGroup g = xTaskGroupCreate(nullptr);
  ASSERT_NE(g, nullptr);
  /* Lazy-loading: threads are created on demand */
  EXPECT_EQ(xTaskGroupThreads(g), (size_t)0);

  xTask t = xTaskSubmit(g, noop, nullptr);
  ASSERT_NE(t, nullptr);
  xTaskWait(t, nullptr);
  EXPECT_GE(xTaskGroupThreads(g), (size_t)1);
  xTaskGroupDestroy(g);
}

/* ========== Return Value ========== */

static void *return_value(void *arg) {
  return arg;
}

TEST_F(TaskTest, WaitReturnsTaskResult) {
  int   val    = 42;
  void *result = nullptr;
  xTask t      = xTaskSubmit(g, return_value, &val);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xTaskWait(t, &result), xErrno_Ok);
  EXPECT_EQ(result, &val);
}

TEST_F(TaskTest, WaitResultNullIgnored) {
  xTask t = xTaskSubmit(g, return_value, nullptr);
  ASSERT_NE(t, nullptr);
  /* Passing NULL for result should not crash */
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);
}

/* ========== Heavy Workload ========== */

TEST_F(TaskTest, StressTest) {
  constexpr int N = 10000;
  Counter       counter;

  for (int i = 0; i < N; i++) {
    xTaskSubmit(g, increment, &counter);
  }

  EXPECT_EQ(xTaskGroupWait(g), xErrno_Ok);
  EXPECT_EQ(counter.value.load(), N);
}

/* ========== Cancel ========== */

TEST_F(TaskTest, CancelNullReturnsError) {
  EXPECT_EQ(xTaskCancel(nullptr), xErrno_InvalidArg);
}

TEST_F(TaskTest, CancelQueuedTask) {
  /* Use a single-threaded group and block the worker so tasks queue up. */
  std::atomic<bool> unblock{false};
  auto              block_fn = [](void *arg) -> void * {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    while (!flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  xTaskGroupConf conf   = {.nthreads = 1, .queue_cap = 0};
  xTaskGroup     single = xTaskGroupCreate(&conf);
  ASSERT_NE(single, nullptr);

  /* Block the worker */
  xTask blocker = xTaskSubmit(single, block_fn, &unblock);
  ASSERT_NE(blocker, nullptr);

  /* Give the worker time to pick up the blocking task */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Submit a task that should stay queued */
  Counter c;
  xTask   t = xTaskSubmit(single, increment, &c);
  ASSERT_NE(t, nullptr);

  /* Cancel should succeed — task is still queued */
  EXPECT_EQ(xTaskCancel(t), xErrno_Ok);

  /* Unblock the worker and wait for everything to drain */
  unblock.store(true, std::memory_order_release);
  xTaskGroupWait(single);

  /* The cancelled task's fn should NOT have been called */
  EXPECT_EQ(c.value.load(), 0);

  /* xTaskWait on a cancelled task should return xErrno_Cancelled */
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Cancelled);

  xTaskGroupDestroy(single);
}

TEST_F(TaskTest, CancelRunningTaskReturnsBusy) {
  /* Submit a long-running task and try to cancel it while it runs. */
  std::atomic<bool> started{false};
  std::atomic<bool> unblock{false};

  struct Ctx {
    std::atomic<bool> *started;
    std::atomic<bool> *unblock;
  };
  Ctx ctx{&started, &unblock};

  auto slow_fn = [](void *arg) -> void * {
    auto *c = static_cast<Ctx *>(arg);
    c->started->store(true, std::memory_order_release);
    while (!c->unblock->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  xTask t = xTaskSubmit(g, slow_fn, &ctx);
  ASSERT_NE(t, nullptr);

  /* Wait until the task is actually running */
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  /* Cancel should fail — task is already running */
  EXPECT_EQ(xTaskCancel(t), xErrno_Busy);

  /* Let it finish and wait */
  unblock.store(true, std::memory_order_release);
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);
}

TEST_F(TaskTest, CancelAlreadyDoneReturnsBusy) {
  xTask t = xTaskSubmit(g, noop, nullptr);
  ASSERT_NE(t, nullptr);

  /* Wait for it to complete first */
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);

  /* Cancel after completion should return Busy */
  EXPECT_EQ(xTaskCancel(t), xErrno_Busy);
}

TEST_F(TaskTest, CancelSafeArgRelease) {
  /* Demonstrates the safe arg-release pattern:
   * cancel succeeds → free arg immediately;
   * cancel fails    → wait, then free. */
  std::atomic<bool> unblock{false};
  auto              block_fn = [](void *arg) -> void * {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    while (!flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  xTaskGroupConf conf   = {.nthreads = 1, .queue_cap = 0};
  xTaskGroup     single = xTaskGroupCreate(&conf);
  ASSERT_NE(single, nullptr);

  /* Block the worker */
  xTask _ = xTaskSubmit(single, block_fn, &unblock);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Submit a task with a heap-allocated arg */
  int  *arg = new int(42);
  xTask t   = xTaskSubmit(single, increment, arg);
  ASSERT_NE(t, nullptr);

  /* Cancel succeeds — safe to free arg now */
  EXPECT_EQ(xTaskCancel(t), xErrno_Ok);
  delete arg; /* No crash — fn was never called */

  unblock.store(true, std::memory_order_release);
  xTaskGroupWait(single);
  xTaskGroupDestroy(single);
}

TEST_F(TaskTest, CancelMultipleQueuedTasks) {
  std::atomic<bool> unblock{false};
  auto              block_fn = [](void *arg) -> void * {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    while (!flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return nullptr;
  };

  xTaskGroupConf conf   = {.nthreads = 1, .queue_cap = 0};
  xTaskGroup     single = xTaskGroupCreate(&conf);
  ASSERT_NE(single, nullptr);

  xTask _ = xTaskSubmit(single, block_fn, &unblock);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Submit several tasks and cancel them all */
  constexpr int          N = 10;
  Counter                counter;
  std::vector<xTask>     tasks(N);

  for (int i = 0; i < N; i++) {
    tasks[i] = xTaskSubmit(single, increment, &counter);
    ASSERT_NE(tasks[i], nullptr);
  }

  for (auto t : tasks) {
    EXPECT_EQ(xTaskCancel(t), xErrno_Ok);
  }

  unblock.store(true, std::memory_order_release);
  xTaskGroupWait(single);

  /* None of the cancelled tasks should have run */
  EXPECT_EQ(counter.value.load(), 0);

  for (auto t : tasks) {
    EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Cancelled);
  }

  xTaskGroupDestroy(single);
}

/* ========== Global Task Group ========== */

TEST(TaskGroupGlobal, ReturnsNonNull) {
  xTaskGroup g = xTaskGroupGlobal();
  ASSERT_NE(g, nullptr);
}

TEST(TaskGroupGlobal, ReturnsSameInstance) {
  xTaskGroup g1 = xTaskGroupGlobal();
  xTaskGroup g2 = xTaskGroupGlobal();
  EXPECT_EQ(g1, g2);
}

TEST(TaskGroupGlobal, CanSubmitAndWait) {
  Counter    c;
  xTaskGroup g = xTaskGroupGlobal();
  ASSERT_NE(g, nullptr);

  xTask t = xTaskSubmit(g, increment, &c);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xTaskWait(t, nullptr), xErrno_Ok);
  EXPECT_EQ(c.value.load(), 1);
}

TEST(TaskGroupGlobal, ConcurrentAccess) {
  constexpr int THREADS    = 4;
  constexpr int PER_THREAD = 100;
  Counter       counter;

  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&]() {
      xTaskGroup g = xTaskGroupGlobal();
      for (int i = 0; i < PER_THREAD; i++) {
        xTaskSubmit(g, increment, &counter);
      }
    });
  }

  for (auto &th : threads)
    th.join();
  xTaskGroupWait(xTaskGroupGlobal());

  EXPECT_EQ(counter.value.load(), THREADS * PER_THREAD);
}
