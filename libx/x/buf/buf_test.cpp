/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buffer_test.cpp - Tests for xBuffer (linear auto-growing buffer)
 */

#include <gtest/gtest.h>

extern "C" {
#include <x/buf/buf.h>
}

#include <cstring>
#include <unistd.h>

/* ───────────────────── Lifecycle ───────────────────── */

TEST(xBuffer, CreateDestroy) {
  xBuffer buf = xBufferCreate(256);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(xBufferCap(buf), 256u);
  EXPECT_EQ(xBufferLen(buf), 0u);
  xBufferDestroy(buf);
}

TEST(xBuffer, CreateDefaultCap) {
  /* Passing 0 should give a default capacity. */
  xBuffer buf = xBufferCreate(0);
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(xBufferCap(buf), 0u);
  EXPECT_EQ(xBufferLen(buf), 0u);
  xBufferDestroy(buf);
}

TEST(xBuffer, DestroyNull) {
  xBufferDestroy(NULL); /* should not crash */
}

/* ───────────────────── Append / Read ───────────────────── */

TEST(xBuffer, AppendAndRead) {
  xBuffer buf = xBufferCreate(64);

  const char *msg = "hello, xBuffer!";
  ASSERT_EQ(xBufferAppend(&buf, msg, strlen(msg)), xErrno_Ok);
  EXPECT_EQ(xBufferLen(buf), strlen(msg));
  EXPECT_EQ(memcmp(xBufferData(buf), msg, strlen(msg)), 0);

  xBufferDestroy(buf);
}

TEST(xBuffer, AppendStr) {
  xBuffer buf = xBufferCreate(64);

  ASSERT_EQ(xBufferAppendStr(&buf, "foo"), xErrno_Ok);
  ASSERT_EQ(xBufferAppendStr(&buf, "bar"), xErrno_Ok);
  EXPECT_EQ(xBufferLen(buf), 6u);
  EXPECT_EQ(memcmp(xBufferData(buf), "foobar", 6), 0);

  xBufferDestroy(buf);
}

TEST(xBuffer, AutoGrow) {
  xBuffer buf = xBufferCreate(8);

  /* Write more than initial capacity. */
  char data[256];
  memset(data, 'A', sizeof(data));
  ASSERT_EQ(xBufferAppend(&buf, data, sizeof(data)), xErrno_Ok);
  EXPECT_EQ(xBufferLen(buf), sizeof(data));
  EXPECT_GE(xBufferCap(buf), sizeof(data));
  EXPECT_EQ(memcmp(xBufferData(buf), data, sizeof(data)), 0);

  xBufferDestroy(buf);
}

/* ───────────────────── Consume / Compact ───────────────────── */

TEST(xBuffer, ConsumePartial) {
  xBuffer buf = xBufferCreate(64);
  xBufferAppend(&buf, "abcdef", 6);

  xBufferConsume(buf, 3);
  EXPECT_EQ(xBufferLen(buf), 3u);
  EXPECT_EQ(memcmp(xBufferData(buf), "def", 3), 0);

  xBufferDestroy(buf);
}

TEST(xBuffer, ConsumeFull) {
  xBuffer buf = xBufferCreate(64);
  xBufferAppend(&buf, "abc", 3);

  xBufferConsume(buf, 100); /* more than available */
  EXPECT_EQ(xBufferLen(buf), 0u);
  EXPECT_EQ(xBufferData(buf), nullptr);

  xBufferDestroy(buf);
}

TEST(xBuffer, Compact) {
  xBuffer buf = xBufferCreate(64);
  xBufferAppend(&buf, "abcdef", 6);
  xBufferConsume(buf, 4);

  size_t writable_before = xBufferWritable(buf);
  xBufferCompact(buf);
  EXPECT_EQ(xBufferLen(buf), 2u);
  EXPECT_EQ(memcmp(xBufferData(buf), "ef", 2), 0);
  EXPECT_GT(xBufferWritable(buf), writable_before);

  xBufferDestroy(buf);
}

TEST(xBuffer, Reset) {
  xBuffer buf = xBufferCreate(64);
  xBufferAppend(&buf, "data", 4);

  xBufferReset(buf);
  EXPECT_EQ(xBufferLen(buf), 0u);
  EXPECT_EQ(xBufferCap(buf), 64u); /* memory kept */

  xBufferDestroy(buf);
}

/* ───────────────────── Reserve ───────────────────── */

TEST(xBuffer, Reserve) {
  xBuffer buf = xBufferCreate(64);

  ASSERT_EQ(xBufferReserve(&buf, 1024), xErrno_Ok);
  EXPECT_GE(xBufferWritable(buf), 1024u);

  xBufferDestroy(buf);
}

/* ───────────────────── I/O helpers ───────────────────── */

TEST(xBuffer, ReadWriteFd) {
  xBuffer wbuf = xBufferCreate(64);
  xBuffer rbuf = xBufferCreate(64);

  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  const char *msg = "pipe test data";
  xBufferAppend(&wbuf, msg, strlen(msg));

  ssize_t written = xBufferWriteFd(wbuf, pipefd[1]);
  ASSERT_GT(written, 0);
  EXPECT_EQ(xBufferLen(wbuf), 0u);

  ssize_t nread = xBufferReadFd(&rbuf, pipefd[0]);
  ASSERT_GT(nread, 0);
  EXPECT_EQ(xBufferLen(rbuf), strlen(msg));
  EXPECT_EQ(memcmp(xBufferData(rbuf), msg, strlen(msg)), 0);

  close(pipefd[0]);
  close(pipefd[1]);
  xBufferDestroy(wbuf);
  xBufferDestroy(rbuf);
}
