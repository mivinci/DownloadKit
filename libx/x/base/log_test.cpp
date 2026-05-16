/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * log_test.cpp - xLog unit tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include <x/base/log.h>
}

/* ── Helpers ── */

struct CapturedLog {
  std::string msg;
  void       *userdata;
  int         count;
};

static void capture_callback(const char *msg, const char *backtrace,
                             void *userdata) {
  (void)backtrace;
  auto *cap     = static_cast<CapturedLog *>(userdata);
  cap->msg      = msg;
  cap->userdata = userdata;
  cap->count++;
}

/* ── Fixture ── */

class LogTest : public ::testing::Test {
protected:
  CapturedLog captured{};

  void SetUp() override {
    captured = {"", nullptr, 0};
    xLogSetCallback(capture_callback, &captured);
  }

  void TearDown() override {
    /* Clear callback to avoid dangling pointers */
    xLogSetCallback(nullptr, nullptr);
  }
};

/* ========== 4.1 Basic callback registration and trigger ========== */

TEST_F(LogTest, BasicCallbackTrigger) {
  xLog(false, "hello %s", "world");
  EXPECT_EQ(captured.msg, "hello world");
  EXPECT_EQ(captured.count, 1);
}

TEST_F(LogTest, FormattedMessage) {
  xLog(false, "error code: %d, file: %s", 42, "main.c");
  EXPECT_EQ(captured.msg, "error code: 42, file: main.c");
  EXPECT_EQ(captured.count, 1);
}

/* ========== 4.2 Userdata passthrough ========== */

TEST_F(LogTest, UserdataPassthrough) {
  xLog(false, "test");
  EXPECT_EQ(captured.userdata, &captured);
}

TEST_F(LogTest, DifferentUserdata) {
  CapturedLog other{};
  xLogSetCallback(capture_callback, &other);
  xLog(false, "msg");
  EXPECT_EQ(other.userdata, &other);
  EXPECT_EQ(other.msg, "msg");
}

/* ========== 4.3 Clear callback (pass NULL) ========== */

TEST_F(LogTest, ClearCallback) {
  /* First verify callback works */
  xLog(false, "before clear");
  EXPECT_EQ(captured.count, 1);

  /* Clear callback */
  xLogSetCallback(nullptr, nullptr);

  /* This should go to stderr, not our callback */
  xLog(false, "after clear");
  EXPECT_EQ(captured.count, 1); /* count should NOT increase */
}

/* ========== 4.4 Multiple callback overrides ========== */

TEST_F(LogTest, OverrideCallback) {
  CapturedLog first{};
  CapturedLog second{};

  xLogSetCallback(capture_callback, &first);
  xLogSetCallback(capture_callback, &second);

  xLog(false, "which one?");

  /* Only the last-set callback should fire */
  EXPECT_EQ(first.count, 0);
  EXPECT_EQ(second.count, 1);
  EXPECT_EQ(second.msg, "which one?");
}

/* ========== 4.5 NULL fmt defense ========== */

TEST_F(LogTest, NullFmtDoesNotCrash) {
  xLog(false, nullptr);
  EXPECT_EQ(captured.msg, "(null)");
  EXPECT_EQ(captured.count, 1);
}

/* ========== 4.6 Message truncation ========== */

TEST_F(LogTest, MessageTruncation) {
  /* Build a string longer than XLOG_BUF_SIZE (512) */
  std::string long_msg(1024, 'A');
  xLog(false, "%s", long_msg.c_str());

  /* Message should be truncated to XLOG_BUF_SIZE - 1 */
  EXPECT_EQ(captured.msg.size(), (size_t)(XLOG_BUF_SIZE - 1));
  EXPECT_EQ(captured.count, 1);

  /* All characters should be 'A' */
  for (char c : captured.msg) {
    EXPECT_EQ(c, 'A');
  }
}

/* ========== 4.7 Thread isolation ========== */

TEST(LogThreadTest, ThreadIsolation) {
  std::atomic<bool> t1_done{false};
  std::atomic<bool> t2_done{false};
  CapturedLog       cap1{};
  CapturedLog       cap2{};

  std::thread thread1([&]() {
    xLogSetCallback(capture_callback, &cap1);
    /* Wait for thread2 to also set its callback */
    while (!t2_done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    xLog(false, "from thread 1");
    t1_done.store(true, std::memory_order_release);
  });

  std::thread thread2([&]() {
    xLogSetCallback(capture_callback, &cap2);
    t2_done.store(true, std::memory_order_release);
    /* Wait for thread1 to finish throwing */
    while (!t1_done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    xLog(false, "from thread 2");
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

TEST(LogThreadTest, NoCallbackInNewThread) {
  /* Main thread sets a callback */
  CapturedLog main_cap{};
  xLogSetCallback(capture_callback, &main_cap);

  std::atomic<bool> child_threw{false};

  std::thread child([&]() {
    /* Child thread has NO callback set — should fallback to stderr */
    xLog(false, "child error");
    child_threw.store(true, std::memory_order_release);
  });

  child.join();
  EXPECT_TRUE(child_threw.load());

  /* Main thread's callback should NOT have been triggered by child */
  EXPECT_EQ(main_cap.count, 0);

  xLogSetCallback(nullptr, nullptr);
}

/* ========== Fatal abort ========== */

/*
 * Death tests that trigger xLog(true, ...) call xBacktraceSkip() which
 * uses backtrace_symbols() (execinfo) or libunwind.  Under ASan the
 * forked child may deadlock in malloc, so we use the "threadsafe"
 * death-test style which re-executes the binary instead of forking.
 */

TEST(LogDeathTest, FatalAbortsProcess) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH({ xLog(true, "fatal error %d", 42); }, "");
}

/* ========== Fatal with callback captures backtrace ========== */

TEST(LogDeathTest, FatalCallbackReceivesBacktrace) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
    {
      xLogSetCallback(
        [](const char *msg, const char *backtrace, void *) {
          (void)msg;
          /* backtrace may or may not be NULL depending on platform */
          fprintf(stderr, "bt=%s\n", backtrace ? "yes" : "no");
        },
        nullptr);
      xLog(true, "fatal with bt");
      /* abort() is called after callback, so we never reach here */
    },
    "");
}

/* ========== Non-fatal callback receives NULL backtrace ========== */

TEST_F(LogTest, NonFatalBacktraceIsNull) {
  static const char *received_bt = nullptr;
  xLogSetCallback(
    [](const char *, const char *backtrace, void *) {
      received_bt = backtrace;
    },
    nullptr);
  xLog(false, "non-fatal");
  EXPECT_EQ(received_bt, nullptr);
}

/* ========== Stderr fallback with fatal (death test) ========== */

TEST(LogDeathTest, StderrFallbackFatal) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
    {
      xLogSetCallback(nullptr, nullptr);
      xLog(true, "stderr fatal");
    },
    "");
}

/* ========== Empty format string ========== */

TEST_F(LogTest, EmptyFormatString) {
  xLog(false, "");
  EXPECT_EQ(captured.msg, "");
  EXPECT_EQ(captured.count, 1);
}

/* ========== Multiple sequential logs ========== */

TEST_F(LogTest, MultipleSequentialLogs) {
  xLog(false, "first");
  EXPECT_EQ(captured.msg, "first");
  EXPECT_EQ(captured.count, 1);

  xLog(false, "second");
  EXPECT_EQ(captured.msg, "second");
  EXPECT_EQ(captured.count, 2);

  xLog(false, "third");
  EXPECT_EQ(captured.msg, "third");
  EXPECT_EQ(captured.count, 3);
}
