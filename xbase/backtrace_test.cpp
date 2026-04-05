/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * backtrace_test.cpp - xBacktrace unit tests
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include <xbase/backtrace.h>
}

/* ── Helpers ── */

static int countFrames(const char *buf) {
  int count = 0;
  const char *p = buf;
  while ((p = strstr(p, "#")) != nullptr) {
    /* Verify it looks like a frame marker: #<digit> */
    if (p[1] >= '0' && p[1] <= '9')
      count++;
    p++;
  }
  return count;
}

/* ========== Basic functionality ========== */

TEST(BacktraceTest, BasicCapture) {
  char buf[4096];
  int n = xBacktrace(buf, sizeof(buf));

  EXPECT_GT(n, 0);
  EXPECT_EQ(n, (int)strlen(buf));

  /* Should contain at least one frame */
  EXPECT_NE(strstr(buf, "#0"), nullptr);
}

TEST(BacktraceTest, OutputContainsFrameMarkers) {
  char buf[4096];
  int n = xBacktrace(buf, sizeof(buf));

  EXPECT_GT(n, 0);
  EXPECT_GE(countFrames(buf), 1);
}

/* ========== NULL / zero-size defense ========== */

TEST(BacktraceTest, NullBufReturnsZero) {
  int n = xBacktrace(nullptr, 1024);
  EXPECT_EQ(n, 0);
}

TEST(BacktraceTest, ZeroSizeReturnsZero) {
  char buf[16];
  int n = xBacktrace(buf, 0);
  EXPECT_EQ(n, 0);
}

TEST(BacktraceTest, NullBufZeroSizeReturnsZero) {
  int n = xBacktrace(nullptr, 0);
  EXPECT_EQ(n, 0);
}

/* ========== Truncation ========== */

TEST(BacktraceTest, SmallBufferTruncation) {
  char buf[16];
  memset(buf, 'X', sizeof(buf));

  int n = xBacktrace(buf, sizeof(buf));

  /* Should be truncated but valid */
  EXPECT_GE(n, 0);
  EXPECT_LT(n, 16);
  EXPECT_EQ(buf[n], '\0');

  /* Verify no buffer overrun */
  EXPECT_LE((size_t)n, sizeof(buf) - 1);
}

TEST(BacktraceTest, ExactOneByteBuf) {
  char buf[1];
  int n = xBacktrace(buf, sizeof(buf));

  /* Only room for NUL terminator */
  EXPECT_EQ(n, 0);
  EXPECT_EQ(buf[0], '\0');
}

/* ========== Skip control ========== */

TEST(BacktraceTest, SkipReducesFrames) {
  char buf0[4096], buf2[4096], buf5[4096];

  int n0 = xBacktraceSkip(0, buf0, sizeof(buf0));
  (void)xBacktraceSkip(2, buf2, sizeof(buf2));
  (void)xBacktraceSkip(5, buf5, sizeof(buf5));

  int frames0 = countFrames(buf0);
  int frames2 = countFrames(buf2);
  int frames5 = countFrames(buf5);

  EXPECT_GT(n0, 0);
  EXPECT_GT(frames0, 0);

  /* More skipping → fewer (or equal) frames */
  EXPECT_LE(frames2, frames0);
  EXPECT_LE(frames5, frames2);
}

TEST(BacktraceTest, SkipAllFrames) {
  char buf[4096];

  /* Skip a very large number — should produce empty or near-empty output */
  int n = xBacktraceSkip(9999, buf, sizeof(buf));
  EXPECT_GE(n, 0);
}

/* ========== xBacktrace equivalence ========== */

TEST(BacktraceTest, BacktraceEqualsSkipZero) {
  char buf1[4096], buf2[4096];

  /*
   * Both should produce similar output (frame counts may differ by 1
   * due to the extra xBacktrace wrapper frame, but the content should
   * be structurally identical).
   */
  int n1 = xBacktrace(buf1, sizeof(buf1));
  int n2 = xBacktraceSkip(0, buf2, sizeof(buf2));

  EXPECT_GT(n1, 0);
  EXPECT_GT(n2, 0);

  /* Both should have frame markers */
  EXPECT_GE(countFrames(buf1), 1);
  EXPECT_GE(countFrames(buf2), 1);
}

/* ========== Format validation ========== */

TEST(BacktraceTest, FrameFormatPattern) {
  char buf[4096];
  int n = xBacktrace(buf, sizeof(buf));

  EXPECT_GT(n, 0);

  /* Each line should start with #N followed by content */
  std::string output(buf);
  std::istringstream stream(output);
  std::string line;
  int frame_idx = 0;

  while (std::getline(stream, line)) {
    if (line.empty())
      continue;

    /* Should start with #<number> */
    EXPECT_EQ(line[0], '#');

    std::string expected_prefix = "#" + std::to_string(frame_idx);
    EXPECT_EQ(line.substr(0, expected_prefix.size()), expected_prefix)
        << "Frame " << frame_idx << " has unexpected prefix: " << line;

    frame_idx++;
  }

  EXPECT_GT(frame_idx, 0);
}
