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
  std::atomic<bool>         work_done{false};
  std::atomic<bool>         done_called{false};
  std::atomic<std::thread::id> work_thread{};
  std::atomic<std::thread::id> done_thread{};
  void                     *result_ptr{nullptr};
};

static void *basic_work(void *arg) {
  auto *ctx = static_cast<OffloadCtx *>(arg);
  ctx->work_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  ctx->work_done.store(true, std::memory_order_release);
  return ctx; /* return a recognisable pointer */
}

static void basic_done(void *arg, void *result) {
  auto *ctx = static_cast<OffloadCtx *>(arg);
  ctx->done_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  ctx->result_ptr = result;
  ctx->done_called.store(true, std::memory_order_release);
}

TEST_F(EventOffloadTest, BasicOffload) {
  OffloadCtx ctx;

  ASSERT_EQ(xEventLoopSubmit(loop, group, basic_work, basic_done, &ctx, nullptr),
            xErrno_Ok);

  /* Pump the event loop until done_fn fires (max 2 s). */
  for (int i = 0; i < 200 && !ctx.done_called.load(std::memory_order_acquire);
       i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(ctx.work_done.load());
  EXPECT_TRUE(ctx.done_called.load());
  /* work_fn must have run on a different thread than the test thread. */
  EXPECT_NE(ctx.work_thread.load(), std::this_thread::get_id());
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

  ASSERT_EQ(xEventLoopSubmit(loop, group, work_fn, nullptr, &work_done, nullptr),
            xErrno_Ok);

  /* Pump the loop to let the done queue drain (even though done_fn is NULL). */
  for (int i = 0; i < 200 && !work_done.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(work_done.load());
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(EventOffloadTest, NullLoopReturnsError) {
  EXPECT_EQ(xEventLoopSubmit(nullptr, group, basic_work, basic_done, nullptr,
                             nullptr),
            xErrno_InvalidArg);
}

TEST_F(EventOffloadTest, NullWorkFnReturnsError) {
  EXPECT_EQ(xEventLoopSubmit(loop, group, nullptr, basic_done, nullptr, nullptr),
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
          &sctx, nullptr);
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
  ASSERT_EQ(xEventLoopSubmit(loop, nullptr, basic_work, basic_done, &ctx,
                             nullptr),
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

  ASSERT_EQ(xEventLoopSubmit(loop, group, work_fn, done_fn, &out, nullptr),
            xErrno_Ok);

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
        &done_count, nullptr);
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
    nullptr, &unblock, nullptr);

  /* Give the worker time to pick up the blocking task */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Fill the queue */
  xEventLoopSubmit(
    loop, small, [](void *) -> void * { return nullptr; }, nullptr, nullptr,
    nullptr);

  /* Next submit should fail because queue is full */
  xErrno err = xEventLoopSubmit(
    loop, small, [](void *) -> void * { return nullptr; }, nullptr, nullptr,
    nullptr);
  EXPECT_EQ(err, xErrno_SysError);

  /* Cleanup */
  unblock.store(true, std::memory_order_release);
  for (int i = 0; i < 200; i++)
    xEventWait(loop, 10);

  xTaskGroupDestroy(small);
}

/* ───────────────────── Cancel offload ───────────────────── */

TEST_F(EventOffloadTest, CancelNullReturnsError) {
  EXPECT_EQ(xEventLoopWorkCancel(loop, nullptr), xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopWorkCancel(nullptr, nullptr), xErrno_InvalidArg);
}

TEST_F(EventOffloadTest, CancelQueuedWork) {
  /* Use a single-threaded group and block the worker so tasks queue up. */
  std::atomic<bool> unblock{false};
  xTaskGroupConf    conf  = {.nthreads = 1, .queue_cap = 0};
  xTaskGroup        small = xTaskGroupCreate(&conf);
  ASSERT_NE(small, nullptr);

  /* Block the worker */
  xEventLoopSubmit(
    loop, small,
    [](void *arg) -> void * {
      auto *flag = static_cast<std::atomic<bool> *>(arg);
      while (!flag->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      return nullptr;
    },
    nullptr, &unblock, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  /* Submit a task that should stay queued, get the work handle */
  std::atomic<bool> work_called{false};
  std::atomic<bool> done_called{false};
  xEventWork        work = nullptr;

  ASSERT_EQ(
    xEventLoopSubmit(
      loop, small,
      [](void *arg) -> void * {
        static_cast<std::atomic<bool> *>(arg)->store(
          true, std::memory_order_release);
        return nullptr;
      },
      [](void *arg, void *) {
        /* This is the done_fn — should NOT be called if cancelled. */
        static_cast<std::atomic<bool> *>(arg)->store(
          true, std::memory_order_release);
      },
      &work_called, &work),
    xErrno_Ok);
  ASSERT_NE(work, nullptr);

  /* Cancel should succeed — task is still queued */
  EXPECT_EQ(xEventLoopWorkCancel(loop, work), xErrno_Ok);

  /* Unblock the worker and pump the loop */
  unblock.store(true, std::memory_order_release);
  for (int i = 0; i < 200; i++)
    xEventWait(loop, 10);

  /* Neither work_fn nor done_fn should have been called */
  EXPECT_FALSE(work_called.load());
  EXPECT_FALSE(done_called.load());

  xTaskGroupDestroy(small);
}

TEST_F(EventOffloadTest, CancelRunningWorkFails) {
  /* Submit a long-running task and try to cancel it while it runs. */
  std::atomic<bool> started{false};
  std::atomic<bool> unblock{false};

  struct Ctx {
    std::atomic<bool> *started;
    std::atomic<bool> *unblock;
  };
  Ctx ctx{&started, &unblock};

  xEventWork work = nullptr;
  ASSERT_EQ(
    xEventLoopSubmit(
      loop, group,
      [](void *arg) -> void * {
        auto *c = static_cast<Ctx *>(arg);
        c->started->store(true, std::memory_order_release);
        while (!c->unblock->load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return nullptr;
      },
      nullptr, &ctx, &work),
    xErrno_Ok);

  /* Wait until the task is actually running */
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  /* Cancel should fail — task is already running */
  EXPECT_EQ(xEventLoopWorkCancel(loop, work), xErrno_InvalidState);

  /* Let it finish and pump the loop */
  unblock.store(true, std::memory_order_release);
  for (int i = 0; i < 200; i++)
    xEventWait(loop, 10);
}

TEST_F(EventOffloadTest, CancelSubmitOutHandle) {
  /* Verify that passing a non-NULL out parameter populates the handle. */
  xEventWork work = nullptr;
  ASSERT_EQ(
    xEventLoopSubmit(
      loop, group,
      [](void *) -> void * { return nullptr; },
      nullptr, nullptr, &work),
    xErrno_Ok);
  EXPECT_NE(work, nullptr);

  /* Pump the loop to let it complete normally */
  for (int i = 0; i < 200; i++)
    xEventWait(loop, 10);
}

TEST_F(EventOffloadTest, CancelSubmitNullOutStillWorks) {
  /* Passing NULL for out should still work (backward compat). */
  std::atomic<bool> done{false};
  ASSERT_EQ(
    xEventLoopSubmit(
      loop, group,
      [](void *) -> void * { return nullptr; },
      [](void *arg, void *) {
        static_cast<std::atomic<bool> *>(arg)->store(
          true, std::memory_order_release);
      },
      &done, nullptr),
    xErrno_Ok);

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); i++)
    xEventWait(loop, 10);

  EXPECT_TRUE(done.load());
}