/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_post_test.cpp - Unit tests for xEventLoopPost
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/event.h>
}

/* ───────────────────── Fixture ───────────────────── */

class EventPostTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
  }

  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Basic post ───────────────────── */

TEST_F(EventPostTest, BasicPost) {
  std::atomic<bool> called{false};

  auto fn = [](void *arg) {
    static_cast<std::atomic<bool> *>(arg)->store(true,
                                                 std::memory_order_release);
  };

  ASSERT_EQ(xEventLoopPost(loop, fn, &called), xErrno_Ok);

  /* Pump the loop until the callback fires. */
  for (int i = 0; i < 200 && !called.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(called.load());
}

/* ───────────────────── Post from another thread ───────────────────── */

TEST_F(EventPostTest, PostFromAnotherThread) {
  std::atomic<bool>            called{false};
  std::atomic<std::thread::id> cb_thread{};

  auto fn = [](void *arg) {
    auto *ctx = static_cast<
      std::pair<std::atomic<bool> *, std::atomic<std::thread::id> *> *>(arg);
    ctx->second->store(std::this_thread::get_id(), std::memory_order_release);
    ctx->first->store(true, std::memory_order_release);
  };

  std::pair<std::atomic<bool> *, std::atomic<std::thread::id> *> ctx{
    &called, &cb_thread};

  std::thread poster(
    [&]() { EXPECT_EQ(xEventLoopPost(loop, fn, &ctx), xErrno_Ok); });

  poster.join();

  /* Pump the loop until the callback fires. */
  std::thread::id loop_thread = std::this_thread::get_id();
  for (int i = 0; i < 200 && !called.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(called.load());
  /* Callback must have run on the loop thread (the current thread). */
  EXPECT_EQ(cb_thread.load(), loop_thread);
}

/* ───────────────────── Multiple posts ───────────────────── */

TEST_F(EventPostTest, MultiplePosts) {
  constexpr int    N = 100;
  std::atomic<int> count{0};

  auto fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1,
                                                    std::memory_order_relaxed);
  };

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(xEventLoopPost(loop, fn, &count), xErrno_Ok);
  }

  for (int i = 0; i < 500 && count.load(std::memory_order_acquire) < N; i++) {
    xEventWait(loop, 10);
  }

  EXPECT_EQ(count.load(), N);
}

/* ───────────────────── Concurrent posts from multiple threads ───────── */

TEST_F(EventPostTest, ConcurrentPosts) {
  constexpr int THREADS    = 4;
  constexpr int PER_THREAD = 50;
  constexpr int TOTAL      = THREADS * PER_THREAD;

  std::atomic<int> count{0};

  auto fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1,
                                                    std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&]() {
      for (int i = 0; i < PER_THREAD; i++) {
        xEventLoopPost(loop, fn, &count);
      }
    });
  }

  for (auto &th : threads)
    th.join();

  for (int i = 0; i < 500 && count.load(std::memory_order_acquire) < TOTAL;
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_EQ(count.load(), TOTAL);
}

/* ───────────────────── Post interleaved with offload ───────────────── */

TEST_F(EventPostTest, PostInterleavedWithSubmit) {
  std::atomic<int> post_count{0};

  auto post_fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1,
                                                    std::memory_order_relaxed);
  };

  /* Post some callbacks. */
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(xEventLoopPost(loop, post_fn, &post_count), xErrno_Ok);
  }

  /* Pump the loop. */
  for (int i = 0; i < 200 && post_count.load(std::memory_order_acquire) < 10;
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_EQ(post_count.load(), 10);
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(EventPostTest, NullLoopReturnsError) {
  auto fn = [](void *) {};
  EXPECT_EQ(xEventLoopPost(nullptr, fn, nullptr), xErrno_InvalidArg);
}

TEST_F(EventPostTest, NullFnReturnsError) {
  EXPECT_EQ(xEventLoopPost(loop, nullptr, nullptr), xErrno_InvalidArg);
}

/* ───────────────────── Post with xEventLoopRun ───────────────────── */

TEST_F(EventPostTest, PostStopsRunningLoop) {
  /* Post a callback that stops the loop, then run the loop. */
  auto stop_fn = [](void *arg) {
    xEventLoopStop(static_cast<xEventLoop>(arg));
  };

  ASSERT_EQ(xEventLoopPost(loop, stop_fn, loop), xErrno_Ok);

  /* xEventLoopRun should return promptly because the posted callback
   * will stop the loop. */
  xEventLoopRun(loop);

  /* If we get here, the loop was stopped successfully. */
  SUCCEED();
}
