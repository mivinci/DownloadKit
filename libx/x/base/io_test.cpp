/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io_test.cpp - xReader / xWriter / xSeeker / xCloser unit tests (POSIX)
 */

#include <gtest/gtest.h>

#ifdef _WIN32
TEST(Io, SkipOnWindows) { GTEST_SKIP() << "IO tests are POSIX-only"; }
#else

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <x/base/io.h>
}

/* ═══════════════════════════════════════════════════════════════════
 *  Mock helpers
 * ═══════════════════════════════════════════════════════════════════
 */

/* A simple buffer-backed reader that serves data from a byte array. */
struct MockReadCtx {
  const char *data;
  size_t      len;
  size_t      pos;
  int         call_count;
  /* If chunk_size > 0, return at most chunk_size bytes per call. */
  size_t chunk_size;
  /* If fail_after >= 0, return -1 (with errno=EIO) after that many
   * successful bytes have been delivered. */
  ssize_t fail_after;
};

static ssize_t mock_read(void *ctx, void *buf, size_t len) {
  auto *m = static_cast<MockReadCtx *>(ctx);
  m->call_count++;

  if (m->fail_after >= 0 && (ssize_t)m->pos >= m->fail_after) {
    errno = EIO;
    return -1;
  }

  size_t avail = m->len - m->pos;
  if (avail == 0) return 0; /* EOF */

  size_t to_read = len;
  if (to_read > avail) to_read = avail;
  if (m->chunk_size > 0 && to_read > m->chunk_size)
    to_read = m->chunk_size;

  memcpy(buf, m->data + m->pos, to_read);
  m->pos += to_read;
  return (ssize_t)to_read;
}

/* A mock writer that records all writev calls. */
struct MockWriteCtx {
  std::vector<char> written;
  int               call_count;
  int               last_iovcnt;
};

static ssize_t mock_writev(void *ctx, const struct iovec *iov, int iovcnt) {
  auto  *m    = static_cast<MockWriteCtx *>(ctx);
  m->call_count++;
  m->last_iovcnt = iovcnt;
  ssize_t total  = 0;
  for (int i = 0; i < iovcnt; i++) {
    const char *p = static_cast<const char *>(iov[i].iov_base);
    m->written.insert(m->written.end(), p, p + iov[i].iov_len);
    total += (ssize_t)iov[i].iov_len;
  }
  return total;
}

/* A mock seeker. */
struct MockSeekCtx {
  off_t current;
  int   call_count;
};

static off_t mock_seek(void *ctx, off_t offset, int whence) {
  auto *m = static_cast<MockSeekCtx *>(ctx);
  m->call_count++;
  switch (whence) {
  case SEEK_SET: m->current = offset; break;
  case SEEK_CUR: m->current += offset; break;
  case SEEK_END:
    /* Simulate a 1000-byte "file" */
    m->current = 1000 + offset;
    break;
  default: return (off_t)-1;
  }
  return m->current;
}

/* A mock closer. */
struct MockCloseCtx {
  int call_count;
  int return_value;
};

static int mock_close(void *ctx) {
  auto *m = static_cast<MockCloseCtx *>(ctx);
  m->call_count++;
  return m->return_value;
}

/* ═══════════════════════════════════════════════════════════════════
 *  xRead tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, ReadForwardsCorrectly) {
  const char  data[] = "hello";
  MockReadCtx ctx    = {data, 5, 0, 0, 0, -1};
  xReader     r      = {mock_read, &ctx};

  char    buf[16] = {};
  ssize_t n       = xRead(r, buf, sizeof(buf));
  EXPECT_EQ(n, 5);
  EXPECT_STREQ(buf, "hello");
  EXPECT_EQ(ctx.call_count, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  xWrite tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, WriteSingleBuffer) {
  MockWriteCtx ctx = {};
  xWriter      w   = {mock_writev, &ctx};

  const char  data[] = "world";
  ssize_t     n      = xWrite(w, data, 5);
  EXPECT_EQ(n, 5);
  EXPECT_EQ(ctx.call_count, 1);
  EXPECT_EQ(ctx.last_iovcnt, 1); /* Single buffer wrapped as 1 iovec */
  ASSERT_EQ(ctx.written.size(), 5u);
  EXPECT_EQ(std::string(ctx.written.begin(), ctx.written.end()), "world");
}

/* ═══════════════════════════════════════════════════════════════════
 *  xWritev tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, WritevMultipleIovecs) {
  MockWriteCtx ctx = {};
  xWriter      w   = {mock_writev, &ctx};

  const char   a[] = "foo";
  const char   b[] = "bar";
  struct iovec iov[2];
  iov[0].iov_base = const_cast<char *>(a);
  iov[0].iov_len  = 3;
  iov[1].iov_base = const_cast<char *>(b);
  iov[1].iov_len  = 3;

  ssize_t n = xWritev(w, iov, 2);
  EXPECT_EQ(n, 6);
  EXPECT_EQ(ctx.call_count, 1);
  EXPECT_EQ(ctx.last_iovcnt, 2);
  ASSERT_EQ(ctx.written.size(), 6u);
  EXPECT_EQ(std::string(ctx.written.begin(), ctx.written.end()), "foobar");
}

/* ═══════════════════════════════════════════════════════════════════
 *  xSeek tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, SeekForwardsCorrectly) {
  MockSeekCtx ctx = {0, 0};
  xSeeker     s   = {mock_seek, &ctx};

  off_t pos = xSeek(s, 42, SEEK_SET);
  EXPECT_EQ(pos, 42);
  EXPECT_EQ(ctx.call_count, 1);

  pos = xSeek(s, 10, SEEK_CUR);
  EXPECT_EQ(pos, 52);
  EXPECT_EQ(ctx.call_count, 2);

  pos = xSeek(s, -100, SEEK_END);
  EXPECT_EQ(pos, 900);
  EXPECT_EQ(ctx.call_count, 3);
}

/* ═══════════════════════════════════════════════════════════════════
 *  xClose tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, CloseForwardsSuccess) {
  MockCloseCtx ctx = {0, 0};
  xCloser      c   = {mock_close, &ctx};

  int ret = xClose(c);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(ctx.call_count, 1);
}

TEST(IoTest, CloseForwardsFailure) {
  MockCloseCtx ctx = {0, -1};
  xCloser      c   = {mock_close, &ctx};

  int ret = xClose(c);
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(ctx.call_count, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  xReadFull tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, ReadFullOneShot) {
  const char  data[] = "abcdefgh";
  MockReadCtx ctx    = {data, 8, 0, 0, 0, -1};
  xReader     r      = {mock_read, &ctx};

  char    buf[8] = {};
  ssize_t n      = xReadFull(r, buf, 8);
  EXPECT_EQ(n, 8);
  EXPECT_EQ(memcmp(buf, "abcdefgh", 8), 0);
}

TEST(IoTest, ReadFullMultipleChunks) {
  const char  data[] = "abcdefghijklmnop";
  MockReadCtx ctx    = {data, 16, 0, 0, 3, -1}; /* 3 bytes per read */
  xReader     r      = {mock_read, &ctx};

  char    buf[16] = {};
  ssize_t n       = xReadFull(r, buf, 16);
  EXPECT_EQ(n, 16);
  EXPECT_EQ(memcmp(buf, "abcdefghijklmnop", 16), 0);
  /* Should have taken ceil(16/3) = 6 calls */
  EXPECT_EQ(ctx.call_count, 6);
}

TEST(IoTest, ReadFullEofEarly) {
  const char  data[] = "short";
  MockReadCtx ctx    = {data, 5, 0, 0, 0, -1};
  xReader     r      = {mock_read, &ctx};

  char    buf[16] = {};
  ssize_t n       = xReadFull(r, buf, 16);
  EXPECT_EQ(n, 5); /* Only 5 bytes available */
  EXPECT_EQ(memcmp(buf, "short", 5), 0);
}

TEST(IoTest, ReadFullError) {
  const char  data[] = "abc";
  MockReadCtx ctx    = {data, 3, 0, 0, 1, 2}; /* Fail after 2 bytes */
  xReader     r      = {mock_read, &ctx};

  char    buf[16] = {};
  ssize_t n       = xReadFull(r, buf, 16);
  EXPECT_EQ(n, -1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  xReadAll tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(IoTest, ReadAllNormal) {
  const char  data[] = "hello world!";
  MockReadCtx ctx    = {data, 12, 0, 0, 0, -1};
  xReader     r      = {mock_read, &ctx};

  void  *out     = NULL;
  size_t out_len = 0;
  int    ret     = xReadAll(r, &out, &out_len);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_len, 12u);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(memcmp(out, "hello world!", 12), 0);
  free(out);
}

TEST(IoTest, ReadAllEmpty) {
  MockReadCtx ctx = {"", 0, 0, 0, 0, -1};
  xReader     r   = {mock_read, &ctx};

  void  *out     = NULL;
  size_t out_len = 0;
  int    ret     = xReadAll(r, &out, &out_len);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_len, 0u);
  ASSERT_NE(out, nullptr); /* Buffer allocated but empty */
  free(out);
}

TEST(IoTest, ReadAllError) {
  const char  data[] = "abc";
  MockReadCtx ctx    = {data, 3, 0, 0, 1, 2}; /* Fail after 2 bytes */
  xReader     r      = {mock_read, &ctx};

  void  *out     = (void *)0xDEAD; /* Sentinel */
  size_t out_len = 999;
  int    ret     = xReadAll(r, &out, &out_len);
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(out_len, 0u);
}

TEST(IoTest, ReadAllLargeWithExpansion) {
  /* Create data larger than initial 4096 to trigger buffer expansion */
  const size_t       total = 10000;
  std::vector<char>  data(total);
  for (size_t i = 0; i < total; i++) {
    data[i] = (char)('A' + (i % 26));
  }

  MockReadCtx ctx = {data.data(), total, 0, 0, 1000, -1}; /* 1000 per read */
  xReader     r   = {mock_read, &ctx};

  void  *out     = NULL;
  size_t out_len = 0;
  int    ret     = xReadAll(r, &out, &out_len);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_len, total);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(memcmp(out, data.data(), total), 0);
  free(out);
}

#endif /* _WIN32 */
