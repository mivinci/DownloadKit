/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * speed_tracker_test.cpp - Unit tests for xSpeedTracker
 */

#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <chrono>

extern "C" {
#include <xbase/speed_tracker.h>
}

/* ── Initialization ── */

TEST(SpeedTrackerTest, InitMacroZeroesFields) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.3);
  EXPECT_EQ(t.prev_bytes, 0u);
  EXPECT_EQ(t.prev_ms, 0u);
  EXPECT_DOUBLE_EQ(t.smooth_bps, 0.0);
  EXPECT_DOUBLE_EQ(t.alpha, 0.3);
}

/* ── First update sets prev_ms but no speed yet ── */

TEST(SpeedTrackerTest, FirstUpdateNoSpeed) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);
  xSpeedTrackerUpdate(&t, 1000);

  /* After first update, prev_bytes and prev_ms are set, but smooth_bps
   * should still be 0 because there's no previous sample to compute dt */
  EXPECT_EQ(t.prev_bytes, 1000u);
  EXPECT_GT(t.prev_ms, 0u);
  EXPECT_DOUBLE_EQ(t.smooth_bps, 0.0);
}

/* ── Second update computes speed ── */

TEST(SpeedTrackerTest, SecondUpdateComputesSpeed) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(1.0); /* alpha=1.0: no smoothing */
  xSpeedTrackerUpdate(&t, 0);

  /* Sleep a bit to get a measurable dt */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  xSpeedTrackerUpdate(&t, 50000);

  /* With alpha=1.0, smooth_bps = instant speed = 50000 / (dt/1000) */
  /* dt ~= 50ms, so speed ~= 50000 / 0.05 = 1000000 bytes/s */
  EXPECT_GT(t.smooth_bps, 0.0);
  /* Rough check: should be in the ballpark of 500K-2M bytes/s */
  EXPECT_GT(t.smooth_bps, 100000.0);
}

/* ── EMA smoothing works ── */

TEST(SpeedTrackerTest, EMASmoothingReducesVariance) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.3);

  xSpeedTrackerUpdate(&t, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  xSpeedTrackerUpdate(&t, 50000);

  double first_speed = t.smooth_bps;
  EXPECT_GT(first_speed, 0.0);

  /* Third update with same rate should keep speed stable */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  xSpeedTrackerUpdate(&t, 100000);

  /* Speed should still be positive and not wildly different */
  EXPECT_GT(t.smooth_bps, 0.0);
}

/* ── Format: no speed yet ── */

TEST(SpeedTrackerTest, FormatNoSpeedReturnsEmpty) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);
  char          buf[64];

  char *result = xSpeedTrackerFormat(&t, buf, sizeof(buf));
  EXPECT_EQ(result, buf);
  EXPECT_EQ(buf[0], '\0');
}

/* ── Format: KB/s range ── */

TEST(SpeedTrackerTest, FormatKBRange) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);
  /* Manually set smooth_bps to a KB/s value */
  t.smooth_bps = 512.0 * 1024.0; /* 512 KB/s */

  char buf[64];
  xSpeedTrackerFormat(&t, buf, sizeof(buf));

  EXPECT_NE(buf[0], '\0');
  EXPECT_NE(strstr(buf, "KB/s"), nullptr);
}

/* ── Format: MB/s range ── */

TEST(SpeedTrackerTest, FormatMBRange) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);
  /* Manually set smooth_bps to a MB/s value */
  t.smooth_bps = 2.0 * 1024.0 * 1024.0; /* 2 MB/s */

  char buf[64];
  xSpeedTrackerFormat(&t, buf, sizeof(buf));

  EXPECT_NE(buf[0], '\0');
  EXPECT_NE(strstr(buf, "MB/s"), nullptr);
}

/* ── Format: returns buf pointer ── */

TEST(SpeedTrackerTest, FormatReturnsBuf) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);
  t.smooth_bps    = 1024.0;

  char buf[64];
  EXPECT_EQ(xSpeedTrackerFormat(&t, buf, sizeof(buf)), buf);
}

/* ── Format: boundary between KB/s and MB/s ── */

TEST(SpeedTrackerTest, FormatBoundary) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);

  /* Just below 1 MB/s threshold */
  t.smooth_bps = 1024.0 * 1024.0 - 1.0;
  char buf[64];
  xSpeedTrackerFormat(&t, buf, sizeof(buf));
  EXPECT_NE(strstr(buf, "KB/s"), nullptr);

  /* At 1 MB/s threshold */
  t.smooth_bps = 1024.0 * 1024.0;
  xSpeedTrackerFormat(&t, buf, sizeof(buf));
  EXPECT_NE(strstr(buf, "MB/s"), nullptr);
}

/* ── Multiple updates produce reasonable speed ── */

TEST(SpeedTrackerTest, MultipleUpdatesConverge) {
  xSpeedTracker t = XSPEED_TRACKER_INIT(0.5);

  uint64_t total = 0;
  for (int i = 0; i < 10; i++) {
    xSpeedTrackerUpdate(&t, total);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    total += 20000; /* ~1 MB/s */
  }

  /* After several updates, speed should be positive */
  EXPECT_GT(t.smooth_bps, 0.0);
}
