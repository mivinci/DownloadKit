/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * time_test.cpp - Unit tests for xMonoMs and xWallMs
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

extern "C" {
#include <x/base/time.h>
}

/* ── xMonoMs: basic monotonicity ── */

TEST(TimeTest, MonoMsIsMonotonic) {
  uint64_t a = xMonoMs();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  uint64_t b = xMonoMs();
  EXPECT_GT(b, a);
}

TEST(TimeTest, MonoMsReturnsNonZero) {
  uint64_t t = xMonoMs();
  EXPECT_GT(t, 0u);
}

TEST(TimeTest, MonoMsElapsedAccuracy) {
  uint64_t start = xMonoMs();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t end     = xMonoMs();
  uint64_t elapsed = end - start;

  /* Should be at least 40ms and no more than 500ms.
   * The upper bound is intentionally generous: on heavily loaded
   * CI runners the scheduler may delay wake-up by hundreds of ms. */
  EXPECT_GE(elapsed, 40u);
  EXPECT_LE(elapsed, 500u);
}

/* ── xWallMs: basic sanity ── */

TEST(TimeTest, WallMsReturnsNonZero) {
  uint64_t t = xWallMs();
  EXPECT_GT(t, 0u);
}

TEST(TimeTest, WallMsIsReasonable) {
  uint64_t t = xWallMs();
  /* Should be after 2020-01-01 00:00:00 UTC = 1577836800000 ms */
  EXPECT_GT(t, 1577836800000u);
}

TEST(TimeTest, WallMsIsMonotonic) {
  uint64_t a = xWallMs();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  uint64_t b = xWallMs();
  EXPECT_GE(b, a);
}

TEST(TimeTest, WallMsElapsedAccuracy) {
  uint64_t start = xWallMs();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t end     = xWallMs();
  uint64_t elapsed = end - start;

  EXPECT_GE(elapsed, 40u);
  EXPECT_LE(elapsed, 500u);
}

/* ── Both clocks return different values ── */

TEST(TimeTest, MonoAndWallAreDifferent) {
  /* Mono is uptime-based, Wall is epoch-based; they should differ */
  uint64_t mono = xMonoMs();
  uint64_t wall = xWallMs();
  EXPECT_NE(mono, wall);
}
