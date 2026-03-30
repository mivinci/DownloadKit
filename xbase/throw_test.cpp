/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * throw_test.cpp - xThrow unit tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include <xbase/throw.h>
}

/* ── Helpers ── */

struct CapturedThrow {
  std::string msg;
  void       *userdata;
  int         count;
};

static void capture_callback(const char *msg, void *userdata) {
  auto *cap    = static_cast<CapturedThrow *>(userdata);
  cap->msg      = msg;
  cap->userdata = userdata;
  cap->count++;
}

/* ── Fixture ── */

class ThrowTest : public ::testing::Test {
protected:
  CapturedThrow captured{};

  void SetUp() override {
    captured = {"", nullptr, 0};
    xThrowSetCallback(capture_callback, &captured);
  }

  void TearDown() override {
    /* Clear callback to avoid dangling pointers */
    xThrowSetCallback(nullptr, nullptr);
  }
};

/* ========== 4.1 Basic callback registration and trigger ========== */

TEST_F(ThrowTest, BasicCallbackTrigger) {
  xThrow(false, "hello %s", "world");
  EXPECT_EQ(captured.msg, "hello world");
  EXPECT_EQ(captured.count, 1);
}

TEST_F(ThrowTest, FormattedMessage) {
  xThrow(false, "error code: %d, file: %s", 42, "main.c");
  EXPECT_EQ(captured.msg, "error code: 42, file: main.c");
  EXPECT_EQ(captured.count, 1);
}

/* ========== 4.2 Userdata passthrough ========== */

TEST_F(ThrowTest, UserdataPassthrough) {
  xThrow(false, "test");
  EXPECT_EQ(captured.userdata, &captured);
}

TEST_F(ThrowTest, DifferentUserdata) {
  int ctx = 99;
  CapturedThrow other{};
  xThrowSetCallback(capture_callback, &other);
  xThrow(false, "msg");
  EXPECT_EQ(other.userdata, &other);
  EXPECT_EQ(other.msg, "msg");
}

/* ========== 4.3 Clear callback (pass NULL) ========== */

TEST_F(ThrowTest, ClearCallback) {
  /* First verify callback works */
  xThrow(false, "before clear");
  EXPECT_EQ(captured.count, 1);

  /* Clear callback */
  xThrowSetCallback(nullptr, nullptr);

  /* This should go to stderr, not our callback */
  xThrow(false, "after clear");
  EXPECT_EQ(captured.count, 1); /* count should NOT increase */
}

/* ========== 4.4 Multiple callback overrides ========== */

TEST_F(ThrowTest, OverrideCallback) {
  CapturedThrow first{};
  CapturedThrow second{};

  xThrowSetCallback(capture_callback, &first);
  xThrowSetCallback(capture_callback, &second);

  xThrow(false, "which one?");

  /* Only the last-set callback should fire */
  EXPECT_EQ(first.count, 0);
  EXPECT_EQ(second.count, 1);
  EXPECT_EQ(second.msg, "which one?");
}

/* ========== 4.5 NULL fmt defense ========== */

TEST_F(ThrowTest, NullFmtDoesNotCrash) {
  xThrow(false, nullptr);
  EXPECT_EQ(captured.msg, "(null)");
  EXPECT_EQ(captured.count, 1);
}

/* ========== 4.6 Message truncation ========== */

TEST_F(ThrowTest, MessageTruncation) {
  /* Build a string longer than XTHROW_BUF_SIZE (512) */
  std::string long_msg(1024, 'A');
  xThrow(false, "%s", long_msg.c_str());

  /* Message should be truncated to XTHROW_BUF_SIZE - 1 */
  EXPECT_EQ(captured.msg.size(), XTHROW_BUF_SIZE - 1);
  EXPECT_EQ(captured.count, 1);

  /* All characters should be 'A' */
  for (char c : captured.msg) {
    EXPECT_EQ(c, 'A');
  }
}

/* ========== 4.7 Thread isolation ========== */

TEST(ThrowThreadTest, ThreadIsolation) {
  std::atomic<bool> t1_done{false};
  std::atomic<bool> t2_done{false};
  CapturedThrow cap1{};
  CapturedThrow cap2{};

  std::thread thread1([&]() {
    xThrowSetCallback(capture_callback, &cap1);
    /* Wait for thread2 to also set its callback */
    while (!t2_done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    xThrow(false, "from thread 1");
    t1_done.store(true, std::memory_order_release);
  });

  std::thread thread2([&]() {
    xThrowSetCallback(capture_callback, &cap2);
    t2_done.store(true, std::memory_order_release);
    /* Wait for thread1 to finish throwing */
    while (!t1_done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    xThrow(false, "from thread 2");
  });

  thread1.join();
  thread2.join();

  /* Each thread should have triggered its own callback */
  EXPECT_EQ(cap1.msg, "from thread 1");
  EXPECT_EQ(cap1.count, 1);
  EXPECT_EQ(cap1.userdata, &cap1);

  EXPECT_EQ(cap2.msg, "from thread 2");
  EXPECT_EQ(cap2.count, 1);
  EXPECT_EQ(cap2.userdata, &cap2);
}

TEST(ThrowThreadTest, NoCallbackInNewThread) {
  /* Main thread sets a callback */
  CapturedThrow main_cap{};
  xThrowSetCallback(capture_callback, &main_cap);

  std::atomic<bool> child_threw{false};

  std::thread child([&]() {
    /* Child thread has NO callback set — should fallback to stderr */
    xThrow(false, "child error");
    child_threw.store(true, std::memory_order_release);
  });

  child.join();
  EXPECT_TRUE(child_threw.load());

  /* Main thread's callback should NOT have been triggered by child */
  EXPECT_EQ(main_cap.count, 0);

  xThrowSetCallback(nullptr, nullptr);
}

/* ========== Fatal abort ========== */

TEST(ThrowDeathTest, FatalAbortsProcess) {
  EXPECT_DEATH({
    xThrow(true, "fatal error %d", 42);
  }, "");
}
