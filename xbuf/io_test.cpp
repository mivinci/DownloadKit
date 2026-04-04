/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * iobuffer_test.cpp - Tests for xIOBuffer (block-chain I/O buffer)
 */

#include <gtest/gtest.h>

extern "C" {
#include <xbuf/io.h>
}

#include <cstring>
#include <unistd.h>
#include <vector>

/* ───────────────────── Lifecycle ───────────────────── */

TEST(xIOBuffer, InitDeinit) {
  xIOBuffer io;
  xIOBufferInit(&io);
  EXPECT_TRUE(xIOBufferEmpty(&io));
  EXPECT_EQ(xIOBufferLen(&io), 0u);
  EXPECT_EQ(xIOBufferRefCount(&io), 0u);
  xIOBufferDeinit(&io);
}

TEST(xIOBuffer, DeinitNull) {
  xIOBufferDeinit(NULL); /* should not crash */
}

/* ───────────────────── Append / Read ───────────────────── */

TEST(xIOBuffer, AppendSmall) {
  xIOBuffer io;
  xIOBufferInit(&io);

  const char *msg = "hello, xIOBuffer!";
  ASSERT_EQ(xIOBufferAppend(&io, msg, strlen(msg)), xErrno_Ok);
  EXPECT_EQ(xIOBufferLen(&io), strlen(msg));
  EXPECT_EQ(xIOBufferRefCount(&io), 1u);

  char out[64] = {};
  size_t n = xIOBufferRead(&io, out, sizeof(out));
  EXPECT_EQ(n, strlen(msg));
  EXPECT_EQ(memcmp(out, msg, strlen(msg)), 0);
  EXPECT_TRUE(xIOBufferEmpty(&io));

  xIOBufferDeinit(&io);
}

TEST(xIOBuffer, AppendStr) {
  xIOBuffer io;
  xIOBufferInit(&io);

  ASSERT_EQ(xIOBufferAppendStr(&io, "foo"), xErrno_Ok);
  ASSERT_EQ(xIOBufferAppendStr(&io, "bar"), xErrno_Ok);
  EXPECT_EQ(xIOBufferLen(&io), 6u);

  char out[8] = {};
  xIOBufferRead(&io, out, 6);
  EXPECT_EQ(memcmp(out, "foobar", 6), 0);

  xIOBufferDeinit(&io);
}

TEST(xIOBuffer, AppendLargeMultiBlock) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Write more than one block worth of data. */
  size_t total = XIOBUFFER_BLOCK_SIZE * 3 + 100;
  std::vector<char> data(total, 'Z');
  ASSERT_EQ(xIOBufferAppend(&io, data.data(), total), xErrno_Ok);
  EXPECT_EQ(xIOBufferLen(&io), total);
  EXPECT_GE(xIOBufferRefCount(&io), 3u);

  /* Read it all back. */
  std::vector<char> out(total);
  size_t n = xIOBufferRead(&io, out.data(), total);
  EXPECT_EQ(n, total);
  EXPECT_EQ(data, out);
  EXPECT_TRUE(xIOBufferEmpty(&io));

  xIOBufferDeinit(&io);
}

/* ───────────────────── AppendIOBuf (zero-copy) ───────────────────── */

TEST(xIOBuffer, AppendIOBuf) {
  xIOBuffer a, b;
  xIOBufferInit(&a);
  xIOBufferInit(&b);

  xIOBufferAppend(&a, "AAA", 3);
  xIOBufferAppend(&b, "BBB", 3);

  ASSERT_EQ(xIOBufferAppendIOBuffer(&a, &b), xErrno_Ok);
  EXPECT_EQ(xIOBufferLen(&a), 6u);
  EXPECT_TRUE(xIOBufferEmpty(&b)); /* b is emptied */

  char out[8] = {};
  xIOBufferRead(&a, out, 6);
  EXPECT_EQ(memcmp(out, "AAABBB", 6), 0);

  xIOBufferDeinit(&a);
  xIOBufferDeinit(&b);
}

/* ───────────────────── Cut (zero-copy split) ───────────────────── */

TEST(xIOBuffer, Cut) {
  xIOBuffer io, header;
  xIOBufferInit(&io);
  xIOBufferInit(&header);

  xIOBufferAppend(&io, "HEADER:BODY_DATA", 16);

  size_t cut = xIOBufferCut(&io, &header, 7); /* "HEADER:" */
  EXPECT_EQ(cut, 7u);
  EXPECT_EQ(xIOBufferLen(&header), 7u);
  EXPECT_EQ(xIOBufferLen(&io), 9u);

  char hdr[8] = {};
  xIOBufferRead(&header, hdr, 7);
  EXPECT_EQ(memcmp(hdr, "HEADER:", 7), 0);

  char body[16] = {};
  xIOBufferRead(&io, body, 9);
  EXPECT_EQ(memcmp(body, "BODY_DATA", 9), 0);

  xIOBufferDeinit(&io);
  xIOBufferDeinit(&header);
}

TEST(xIOBuffer, CutMultiBlock) {
  xIOBuffer io, dst;
  xIOBufferInit(&io);
  xIOBufferInit(&dst);

  /* Fill across multiple blocks. */
  size_t total = XIOBUFFER_BLOCK_SIZE * 2 + 500;
  std::vector<char> data(total);
  for (size_t i = 0; i < total; i++)
    data[i] = (char)(i & 0xFF);
  xIOBufferAppend(&io, data.data(), total);

  /* Cut first block + partial second. */
  size_t cut_size = XIOBUFFER_BLOCK_SIZE + 100;
  size_t cut = xIOBufferCut(&io, &dst, cut_size);
  EXPECT_EQ(cut, cut_size);
  EXPECT_EQ(xIOBufferLen(&dst), cut_size);
  EXPECT_EQ(xIOBufferLen(&io), total - cut_size);

  /* Verify cut data. */
  std::vector<char> out(cut_size);
  xIOBufferRead(&dst, out.data(), cut_size);
  EXPECT_EQ(memcmp(out.data(), data.data(), cut_size), 0);

  /* Verify remaining data. */
  std::vector<char> rest(total - cut_size);
  xIOBufferRead(&io, rest.data(), total - cut_size);
  EXPECT_EQ(memcmp(rest.data(), data.data() + cut_size, total - cut_size), 0);

  xIOBufferDeinit(&io);
  xIOBufferDeinit(&dst);
}

/* ───────────────────── Consume ───────────────────── */

TEST(xIOBuffer, Consume) {
  xIOBuffer io;
  xIOBufferInit(&io);
  xIOBufferAppend(&io, "abcdefgh", 8);

  size_t n = xIOBufferConsume(&io, 3);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(xIOBufferLen(&io), 5u);

  char out[8] = {};
  xIOBufferRead(&io, out, 5);
  EXPECT_EQ(memcmp(out, "defgh", 5), 0);

  xIOBufferDeinit(&io);
}

/* ───────────────────── CopyTo ───────────────────── */

TEST(xIOBuffer, CopyTo) {
  xIOBuffer io;
  xIOBufferInit(&io);
  xIOBufferAppend(&io, "copy_test", 9);

  char out[16] = {};
  size_t n = xIOBufferCopyTo(&io, out);
  EXPECT_EQ(n, 9u);
  EXPECT_EQ(memcmp(out, "copy_test", 9), 0);
  EXPECT_EQ(xIOBufferLen(&io), 9u); /* not consumed */

  xIOBufferDeinit(&io);
}

/* ───────────────────── Reset ───────────────────── */

TEST(xIOBuffer, Reset) {
  xIOBuffer io;
  xIOBufferInit(&io);
  xIOBufferAppend(&io, "data", 4);

  xIOBufferReset(&io);
  EXPECT_TRUE(xIOBufferEmpty(&io));
  EXPECT_EQ(xIOBufferRefCount(&io), 0u);

  xIOBufferDeinit(&io);
}

/* ───────────────────── I/O helpers ───────────────────── */

TEST(xIOBuffer, ReadWriteFd) {
  xIOBuffer wio, rio;
  xIOBufferInit(&wio);
  xIOBufferInit(&rio);

  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  const char *msg = "iobuf pipe test";
  xIOBufferAppend(&wio, msg, strlen(msg));

  ssize_t written = xIOBufferWriteFd(&wio, pipefd[1]);
  ASSERT_GT(written, 0);
  EXPECT_TRUE(xIOBufferEmpty(&wio));

  ssize_t nread = xIOBufferReadFd(&rio, pipefd[0]);
  ASSERT_GT(nread, 0);
  EXPECT_EQ(xIOBufferLen(&rio), strlen(msg));

  char out[64] = {};
  xIOBufferRead(&rio, out, sizeof(out));
  EXPECT_EQ(memcmp(out, msg, strlen(msg)), 0);

  close(pipefd[0]);
  close(pipefd[1]);
  xIOBufferDeinit(&wio);
  xIOBufferDeinit(&rio);
}

/* ───────────────────── Block pool ───────────────────── */

TEST(xIOBuffer, BlockPoolWarmupDrain) {
  ASSERT_EQ(xIOBlockPoolWarmup(16), xErrno_Ok);

  /* Use some blocks. */
  xIOBuffer io;
  xIOBufferInit(&io);
  xIOBufferAppend(&io, "pool test", 9);
  xIOBufferDeinit(&io);

  /* Drain should not crash. */
  xIOBlockPoolDrain();
}

/* ───────────────────── ReadIov ───────────────────── */

TEST(xIOBuffer, ReadIov) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Create multi-block data. */
  size_t total = XIOBUFFER_BLOCK_SIZE + 100;
  std::vector<char> data(total, 'Q');
  xIOBufferAppend(&io, data.data(), total);

  struct iovec iov[8];
  int cnt = xIOBufferReadIov(&io, iov, 8);
  EXPECT_GE(cnt, 2); /* at least 2 blocks */

  /* Verify total iov length matches. */
  size_t iov_total = 0;
  for (int i = 0; i < cnt; i++)
    iov_total += iov[i].iov_len;
  EXPECT_EQ(iov_total, total);

  xIOBufferDeinit(&io);
}
