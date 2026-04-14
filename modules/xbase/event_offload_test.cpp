/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_offload_test.cpp - Unit tests for xEventLoopSubmit (async offload)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/event.h>
#include <xbase/task.h>
}

/* ───────────────────── Fixture ───────────────────── */

class EventOffloadTest : public ::testing::Test {
protected:
  xEventLoop loop  = nullptr;
  xTaskGroup group = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    xTaskGroupConf conf = {};
    conf.nthreads       = 4;
    group               = xTaskGroupCreate(&conf);
    ASSERT_NE(group, nullptr);
  }

  void TearDown() override {
    if (group) xTaskGroupDestroy(group);
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Basic offload ───────────────────── */

struct OffloadCtx {
  std::atomic<bool>      work_done{false};
  std::atomic<bool>      done_called{false};
  std::atomic<pthread_t> work_thread{0};
  std::atomic<pthread_t> done_thread{0};
  void                  *result_ptr{nullptr};
};

static void *basic_work(void *arg) {
  auto *ctx = static_cast<OffloadCtx *>(arg);
  ctx->work_thread.store(pthread_self(), std::memory_order_relaxed);
  ctx->work_done.store(true, std::memory_order_release);
  return ctx; /* return a recognisable pointer */
}

static void basic_done(void *arg, void *result) {
  auto *ctx = static_cast<OffloadCtx *>(arg);
  ctx->done_thread.store(pthread_self(), std::memory_order_relaxed);
  ctx->result_ptr = result;
  ctx->done_called.store(true, std::memory_order_release);
}

TEST_F(EventOffloadTest, BasicOffload) {
  OffloadCtx ctx;

  ASSERT_EQ(xEventLoopSubmit(loop, group, basic_work, basic_done, &ctx),
            xErrno_Ok);

  /* Pump the event loop until done_fn fires (max 2 s). */
  for (int i = 0; i < 200 && !ctx.done_called.load(std::memory_order_acquire);
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(ctx.work_done.load());
  EXPECT_TRUE(ctx.done_called.load());
  /* work_fn must have run on a different thread than the test thread. */
  EXPECT_NE(ctx.work_thread.load(), pthread_self());
  /* done_fn receives the return value of work_fn. */
  EXPECT_EQ(ctx.result_ptr, &ctx);
}

/* ───────────────────── Fire-and-forget ───────────────────── */

TEST_F(EventOffloadTest, FireAndForget) {
  std::atomic<bool> work_done{false};

  auto work_fn = [](void *arg) -> void * {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    flag->store(true, std::memory_order_release);
    return nullptr;
  };

  ASSERT_EQ(xEventLoopSubmit(loop, group, work_fn, nullptr, &work_done),
            xErrno_Ok);

  /* Pump the loop to let the done queue drain (even though done_fn is NULL). */
  for (int i = 0; i < 200 && !work_done.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(work_done.load());
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(EventOffloadTest, NullLoopReturnsError) {
  EXPECT_EQ(xEventLoopSubmit(nullptr, group, basic_work, basic_done, nullptr),
            xErrno_InvalidArg);
}

TEST_F(EventOffloadTest, NullWorkFnReturnsError) {
  EXPECT_EQ(xEventLoopSubmit(loop, group, nullptr, basic_done, nullptr),
            xErrno_InvalidArg);
}

/* ───────────────────── Concurrent submits ───────────────────── */

TEST_F(EventOffloadTest, ConcurrentSubmits) {
  constexpr int THREADS    = 4;
  constexpr int PER_THREAD = 50;
  constexpr int TOTAL      = THREADS * PER_THREAD;

  std::atomic<int> work_count{0};
  std::atomic<int> done_count{0};

  struct SubmitCtx {
    std::atomic<int> *work_cnt;
    std::atomic<int> *done_cnt;
  };

  SubmitCtx sctx{&work_count, &done_count};

  /* Submit from multiple threads concurrently. */
  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&]() {
      for (int i = 0; i < PER_THREAD; i++) {
        xEventLoopSubmit(
          loop, group,
          [](void *arg) -> void * {
            auto *ctx = static_cast<SubmitCtx *>(arg);
            ctx->work_cnt->fetch_add(1, std::memory_order_relaxed);
            return nullptr;
          },
          [](void *arg, void *) {
            auto *ctx = static_cast<SubmitCtx *>(arg);
            ctx->done_cnt->fetch_add(1, std::memory_order_relaxed);
          },
          &sctx);
      }
    });
  }

  for (auto &th : threads)
    th.join();

  /* Pump the event loop until all done callbacks fire. */
  for (int i = 0; i < 500 && done_count.load(std::memory_order_acquire) < TOTAL;
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_EQ(work_count.load(), TOTAL);
  EXPECT_EQ(done_count.load(), TOTAL);
}

/* ───────────────────── Global group (NULL group) ───────────────────── */

TEST_F(EventOffloadTest, NullGroupUsesGlobal) {
  OffloadCtx ctx;

  /* Pass NULL as group — should use xTaskGroupGlobal(). */
  ASSERT_EQ(xEventLoopSubmit(loop, nullptr, basic_work, basic_done, &ctx),
            xErrno_Ok);

  for (int i = 0; i < 200 && !ctx.done_called.load(std::memory_order_acquire);
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(ctx.work_done.load());
  EXPECT_TRUE(ctx.done_called.load());
  EXPECT_EQ(ctx.result_ptr, &ctx);
}

/* ───────────────────── Result passing ───────────────────── */

TEST_F(EventOffloadTest, ResultPassedToDoneFn) {
  static int        sentinel = 42;
  std::atomic<bool> done{false};
  void             *received_result = nullptr;

  auto work_fn = [](void *) -> void * { return &sentinel; };

  auto done_fn = [](void *arg, void *result) {
    /* arg points to a small struct with the two out-params. */
    struct Out {
      std::atomic<bool> *flag;
      void             **result_slot;
    };
    auto *out         = static_cast<Out *>(arg);
    *out->result_slot = result;
    out->flag->store(true, std::memory_order_release);
  };

  struct Out {
    std::atomic<bool> *flag;
    void             **result_slot;
  } out{&done, &received_result};

  ASSERT_EQ(xEventLoopSubmit(loop, group, work_fn, done_fn, &out), xErrno_Ok);

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(done.load());
  EXPECT_EQ(received_result, &sentinel);
}

/* ───────────────────── Work freelist reuse ───────────────────── */

TEST_F(EventOffloadTest, WorkFreelistReuse) {
  /* Submit multiple rounds of offload work. After the first round completes,
   * the work items should be recycled into the freelist. The second round
   * should reuse them (covering event_work_alloc from freelist path). */
  constexpr int ROUNDS   = 3;
  constexpr int PER_ROUND = 10;

  for (int r = 0; r < ROUNDS; r++) {
    std::atomic<int> done_count{0};

    for (int i = 0; i < PER_ROUND; i++) {
      xEventLoopSubmit(
        loop, group,
        [](void *) -> void * { return nullptr; },
        [](void *arg, void *) {
          static_cast<std::atomic<int> *>(arg)->fetch_add(1,
                                                          std::memory_order_relaxed);
        },
        &done_count);
    }

    /* Pump until all done callbacks fire */
    for (int i = 0; i < 200 &&
                    done_count.load(std::memory_order_acquire) < PER_ROUND;
         i++) {
      xEventWait(loop, 10);
    }

    EXPECT_EQ(done_count.load(), PER_ROUND) << "round " << r;
  }
}

/* ───────────────────── Submit with queue-capped group ───────────────────── */

TEST_F(EventOffloadTest, SubmitFailsWhenGroupFull) {
  /* Create a group with 1 thread and queue cap of 1 */
  xTaskGroupConf conf = {.nthreads = 1, .queue_cap = 1};
  xTaskGroup     small = xTaskGroupCreate(&conf);
  ASSERT_NE(small, nullptr);

  /* Block the single worker */
  std::atomic<bool> unblock{false};
  xEventLoopSubmit(
    loop, small,
    [](void *arg) -> void * {
      auto *flag = static_cast<std::atomic<bool> *>(arg);
      while (!flag->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      return nullptr;
    },
    nullptr, &unblock);

  /* Give the worker time to pick up the blocking task */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Fill the queue */
  xEventLoopSubmit(
    loop, small, [](void *) -> void * { return nullptr; }, nullptr, nullptr);

  /* Next submit should fail because queue is full */
  xErrno err = xEventLoopSubmit(
    loop, small, [](void *) -> void * { return nullptr; }, nullptr, nullptr);
  EXPECT_EQ(err, xErrno_SysError);

  /* Cleanup */
  unblock.store(true, std::memory_order_release);
  for (int i = 0; i < 200; i++)
    xEventWait(loop, 10);

  xTaskGroupDestroy(small);
}
