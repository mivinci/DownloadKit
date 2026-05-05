/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ringbuffer_test.cpp - Tests for xRingBuffer (fixed-size ring buffer)
 */

#include <gtest/gtest.h>

extern "C" {
#include <xbuf/ring.h>
}

#include <cstring>
#include <unistd.h>

/* ───────────────────── Lifecycle ───────────────────── */

TEST(xRingBuffer, CreateDestroy) {
  xRingBuffer rb = xRingBufferCreate(100);
  ASSERT_NE(rb, nullptr);
  /* Capacity is rounded up to next power of two. */
  EXPECT_GE(xRingBufferCap(rb), 100u);
  EXPECT_EQ(xRingBufferCap(rb) & (xRingBufferCap(rb) - 1), 0u); /* power of 2 */
  EXPECT_TRUE(xRingBufferEmpty(rb));
  xRingBufferDestroy(rb);
}

TEST(xRingBuffer, CreateInvalidArg) {
  EXPECT_EQ(xRingBufferCreate(0), nullptr);
}

/* ───────────────────── Write / Read ───────────────────── */

TEST(xRingBuffer, WriteRead) {
  xRingBuffer rb = xRingBufferCreate(64);

  const char *msg = "hello ring";
  ASSERT_EQ(xRingBufferWrite(rb, msg, strlen(msg)), strlen(msg));
  EXPECT_EQ(xRingBufferLen(rb), strlen(msg));

  char   out[32] = {};
  size_t n       = xRingBufferRead(rb, out, sizeof(out));
  EXPECT_EQ(n, strlen(msg));
  EXPECT_EQ(memcmp(out, msg, strlen(msg)), 0);
  EXPECT_TRUE(xRingBufferEmpty(rb));

  xRingBufferDestroy(rb);
}

TEST(xRingBuffer, WrapAround) {
  xRingBuffer rb = xRingBufferCreate(16); /* actual cap = 16 */

  /* Fill most of the buffer. */
  char fill[12];
  memset(fill, 'A', sizeof(fill));
  ASSERT_EQ(xRingBufferWrite(rb, fill, sizeof(fill)), sizeof(fill));

  /* Consume some to advance tail. */
  char tmp[8];
  xRingBufferRead(rb, tmp, 8);

  /* Now write data that wraps around. */
  char wrap[] = "WRAP";
  ASSERT_EQ(xRingBufferWrite(rb, wrap, 4), 4u);

  /* Read everything out. */
  char   out[16] = {};
  size_t n       = xRingBufferRead(rb, out, sizeof(out));
  EXPECT_EQ(n, 4u + 4u); /* 4 remaining from fill + 4 from wrap */
  EXPECT_EQ(memcmp(out + 4, "WRAP", 4), 0);

  xRingBufferDestroy(rb);
}

TEST(xRingBuffer, Full) {
  xRingBuffer rb  = xRingBufferCreate(16);
  size_t      cap = xRingBufferCap(rb);

  char data[16];
  memset(data, 'X', (size_t)cap);
  ASSERT_EQ(xRingBufferWrite(rb, data, cap), cap);
  EXPECT_TRUE(xRingBufferFull(rb));

  /* Writing more should return 0 (no space). */
  EXPECT_EQ(xRingBufferWrite(rb, "a", 1), 0u);

  xRingBufferDestroy(rb);
}

/* ───────────────────── Peek / Discard ───────────────────── */

TEST(xRingBuffer, PeekDiscard) {
  xRingBuffer rb = xRingBufferCreate(64);
  xRingBufferWrite(rb, "abcdef", 6);

  char   out[4] = {};
  size_t n      = xRingBufferPeek(rb, out, 4);
  EXPECT_EQ(n, 4u);
  EXPECT_EQ(memcmp(out, "abcd", 4), 0);
  EXPECT_EQ(xRingBufferLen(rb), 6u); /* not consumed */

  n = xRingBufferDiscard(rb, 3);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(xRingBufferLen(rb), 3u);

  xRingBufferDestroy(rb);
}

/* ───────────────────── I/O helpers ───────────────────── */

TEST(xRingBuffer, ReadWriteFd) {
  xRingBuffer wrb = xRingBufferCreate(256);
  xRingBuffer rrb = xRingBufferCreate(256);

  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  const char *msg = "ring pipe test";
  xRingBufferWrite(wrb, msg, strlen(msg));

  ssize_t written = xRingBufferWriteFd(wrb, pipefd[1]);
  ASSERT_GT(written, 0);
  EXPECT_TRUE(xRingBufferEmpty(wrb));

  ssize_t nread = xRingBufferReadFd(rrb, pipefd[0]);
  ASSERT_GT(nread, 0);
  EXPECT_EQ(xRingBufferLen(rrb), strlen(msg));

  char out[64] = {};
  xRingBufferRead(rrb, out, sizeof(out));
  EXPECT_EQ(memcmp(out, msg, strlen(msg)), 0);

  close(pipefd[0]);
  close(pipefd[1]);
  xRingBufferDestroy(wrb);
  xRingBufferDestroy(rrb);
}

TEST(xRingBuffer, Reset) {
  xRingBuffer rb = xRingBufferCreate(64);
  xRingBufferWrite(rb, "data", 4);

  xRingBufferReset(rb);
  EXPECT_TRUE(xRingBufferEmpty(rb));
  EXPECT_GE(xRingBufferCap(rb), 64u); /* memory kept */

  xRingBufferDestroy(rb);
}
