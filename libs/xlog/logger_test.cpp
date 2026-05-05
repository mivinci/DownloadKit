/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * logger_test.cpp - Unit tests for the xlog async logger
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/event.h>
#include <xbase/log.h>
#include <xlog/logger.h>
}

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

/** Read entire file into a string. */
static std::string read_file(const char *path) {
  std::ifstream      ifs(path);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

/** Remove a file and its rotated variants. */
static void cleanup_files(const char *path, int max_files = 10) {
  std::remove(path);
  for (int i = 1; i < max_files; i++) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s.%d", path, i);
    std::remove(buf);
  }
}

/** Run the event loop for up to `timeout_ms`, pumping events. */
static void pump_loop(xEventLoop loop, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + ms(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    xEventWait(loop, 10);
  }
}

/* ───────────────────── Fixture ───────────────────── */

class LoggerTest : public ::testing::Test {
protected:
  xEventLoop  loop      = nullptr;
  const char *test_path = "/tmp/xlog_test.log";

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    cleanup_files(test_path);
  }

  void TearDown() override {
    cleanup_files(test_path);
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ========== Lifecycle Tests ========== */

TEST_F(LoggerTest, CreateWithValidConf) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Timer;
  conf.level       = xLogLevel_Info;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);
  xLoggerDestroy(logger);
}

TEST_F(LoggerTest, CreateWithNullLoopReturnsNull) {
  xLoggerConf conf = {};
  conf.loop        = nullptr;
  conf.path        = test_path;

  xLogger logger = xLoggerCreate(conf);
  EXPECT_EQ(logger, nullptr);
}

TEST_F(LoggerTest, DestroyNullIsNoop) {
  xLoggerDestroy(nullptr); /* Should not crash */
}

TEST_F(LoggerTest, CreateStderrMode) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = nullptr; /* stderr */
  conf.mode        = xLogMode_Timer;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);
  xLoggerDestroy(logger);
}

/* ========== Level Filtering Tests ========== */

TEST_F(LoggerTest, LevelFiltering) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Warn;
  conf.flush_interval_ms = 10;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* These should be filtered out */
  XLOG_DEBUG_L(logger, "debug message");
  XLOG_INFO_L(logger, "info message");

  /* These should pass through */
  XLOG_WARN_L(logger, "warn message");
  XLOG_ERROR_L(logger, "error message");

  /* Pump loop to let timer flush */
  pump_loop(loop, 100);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_EQ(content.find("debug message"), std::string::npos);
  EXPECT_EQ(content.find("info message"), std::string::npos);
  EXPECT_NE(content.find("warn message"), std::string::npos);
  EXPECT_NE(content.find("error message"), std::string::npos);
}

/* ========== Timer Mode Tests ========== */

TEST_F(LoggerTest, TimerModeFlush) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 20;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  XLOG_INFO_L(logger, "timer test message");

  /* Pump loop to let timer fire */
  pump_loop(loop, 100);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("timer test message"), std::string::npos);
  EXPECT_NE(content.find("INFO"), std::string::npos);
}

/* ========== Notify Mode Tests ========== */

TEST_F(LoggerTest, NotifyModeFlush) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Notify;
  conf.level       = xLogLevel_Debug;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  XLOG_INFO_L(logger, "notify test message");

  /* Pump loop briefly — notify should be fast */
  pump_loop(loop, 50);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("notify test message"), std::string::npos);
}

/* ========== Mixed Mode Tests ========== */

TEST_F(LoggerTest, MixedModeErrorFlushesImmediately) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Mixed;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 5000; /* Very long timer */

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* Error should trigger immediate pipe notification */
  XLOG_ERROR_L(logger, "urgent error");

  /* Short pump — should be enough for pipe-based flush */
  pump_loop(loop, 50);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("urgent error"), std::string::npos);
}

TEST_F(LoggerTest, MixedModeDebugWaitsForTimer) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Mixed;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 30;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  XLOG_DEBUG_L(logger, "delayed debug");

  /* Pump long enough for timer to fire */
  pump_loop(loop, 100);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("delayed debug"), std::string::npos);
}

/* ========== File Rotation Tests ========== */

TEST_F(LoggerTest, FileRotation) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 10;
  conf.max_size          = 100; /* Very small to trigger rotation */
  conf.max_files         = 3;   /* Keep path + path.1 + path.2 */

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* Write enough entries to trigger multiple rotations */
  for (int i = 0; i < 20; i++) {
    XLOG_INFO_L(logger, "rotation test entry %d with some padding text", i);
  }

  pump_loop(loop, 200);
  xLoggerDestroy(logger);

  /* Check that rotated files exist */
  char path1[512], path2[512];
  snprintf(path1, sizeof(path1), "%s.1", test_path);
  snprintf(path2, sizeof(path2), "%s.2", test_path);

  std::ifstream f0(test_path);
  std::ifstream f1(path1);
  EXPECT_TRUE(f0.good()) << "Current log file should exist";
  EXPECT_TRUE(f1.good()) << "Rotated file .1 should exist";
}

/* ========== Stderr Output Tests ========== */

TEST_F(LoggerTest, StderrNoRotation) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = nullptr; /* stderr */
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 10;
  conf.max_size          = 10;
  conf.max_files         = 3;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* Should not crash even with rotation settings */
  XLOG_INFO_L(logger, "stderr test");

  pump_loop(loop, 50);
  xLoggerDestroy(logger);
}

/* ========== Multi-thread Safety Tests ========== */

TEST_F(LoggerTest, MultiThreadSafety) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Notify;
  conf.level       = xLogLevel_Debug;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  constexpr int    THREADS    = 4;
  constexpr int    PER_THREAD = 50;
  std::atomic<int> count{0};

  /* Spawn writer threads */
  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < PER_THREAD; i++) {
        XLOG_INFO_L(logger, "thread %d entry %d", t, i);
        count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  /* Pump loop while writers are active */
  for (auto &th : threads) {
    while (!th.joinable())
      sleep_ms(1);
  }

  /* Wait for all writers to finish */
  for (auto &th : threads)
    th.join();

  /* Pump loop to flush remaining entries */
  pump_loop(loop, 200);
  xLoggerDestroy(logger);

  EXPECT_EQ(count.load(), THREADS * PER_THREAD);

  /* Verify file has content (all entries should be present) */
  std::string content = read_file(test_path);
  EXPECT_FALSE(content.empty());

  /* Count newlines to verify all entries were written */
  int lines = 0;
  for (char c : content) {
    if (c == '\n') lines++;
  }
  EXPECT_EQ(lines, THREADS * PER_THREAD);
}

/* ========== Bridge Tests ========== */

TEST_F(LoggerTest, BridgeRedirectsXLog) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Notify;
  conf.level       = xLogLevel_Debug;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* Enter bridge context */
  xLoggerEnter(logger);

  /* Use xLog() — should be redirected to our logger */
  xLog(false, "bridged message from xLog");

  /* Pump loop */
  pump_loop(loop, 100);

  /* Leave bridge context */
  xLoggerLeave();

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("bridged message from xLog"), std::string::npos);
}

TEST_F(LoggerTest, BridgeLeaveRestoresDefault) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Notify;
  conf.level       = xLogLevel_Debug;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  xLoggerEnter(logger);
  xLoggerLeave();

  /* After leave, xLog should go to stderr, not to our file */
  xLog(false, "should go to stderr not file");

  pump_loop(loop, 100);
  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_EQ(content.find("should go to stderr not file"), std::string::npos);
}

/* ========== Timestamp Format Tests ========== */

TEST_F(LoggerTest, LogEntryContainsTimestamp) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 10;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  XLOG_INFO_L(logger, "timestamp check");

  pump_loop(loop, 100);
  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  /* Should contain a timestamp like "2025-04-03 12:34:56.789" */
  EXPECT_NE(content.find("-"), std::string::npos);
  EXPECT_NE(content.find(":"), std::string::npos);
  EXPECT_NE(content.find("."), std::string::npos);
  EXPECT_NE(content.find("INFO"), std::string::npos);
}

/* ========== Default Flush Interval Tests ========== */

TEST_F(LoggerTest, DefaultFlushInterval) {
  xLoggerConf conf       = {};
  conf.loop              = loop;
  conf.path              = test_path;
  conf.mode              = xLogMode_Timer;
  conf.level             = xLogLevel_Debug;
  conf.flush_interval_ms = 0; /* Should use default 50ms */

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  XLOG_INFO_L(logger, "default interval test");

  pump_loop(loop, 200);
  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("default interval test"), std::string::npos);
}

/* ========== Context Macro Tests (xLoggerCurrent) ========== */

TEST_F(LoggerTest, ContextMacrosUseCurrentLogger) {
  xLoggerConf conf = {};
  conf.loop        = loop;
  conf.path        = test_path;
  conf.mode        = xLogMode_Notify;
  conf.level       = xLogLevel_Debug;

  xLogger logger = xLoggerCreate(conf);
  ASSERT_NE(logger, nullptr);

  /* Enter context */
  xLoggerEnter(logger);
  EXPECT_EQ(xLoggerCurrent(), logger);

  /* Use context macros (no logger parameter) */
  XLOG_INFO("context info message");
  XLOG_WARN("context warn message");

  pump_loop(loop, 100);

  /* Leave context */
  xLoggerLeave();
  EXPECT_EQ(xLoggerCurrent(), nullptr);

  xLoggerDestroy(logger);

  std::string content = read_file(test_path);
  EXPECT_NE(content.find("context info message"), std::string::npos);
  EXPECT_NE(content.find("context warn message"), std::string::npos);
}
