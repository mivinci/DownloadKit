/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_cache_test.cpp - Tests for xfer_cache (buffered write cache)
 *
 * Tests the mmap-backed write cache: basic put, batch flushing,
 * small-file fallback (no mmap), sync vs async flush, and destroy
 * without explicit flush.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xfer/xfer_cache.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

/* ───────────────────── Helpers ───────────────────── */

static char *make_temp_file(const char *name, size_t size) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/%s", name);

  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return NULL;
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return NULL;
  }
  close(fd);
  return strdup(path);
}

static bool verify_chunk(const char *path, uint64_t offset, size_t len,
                         uint8_t fill) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    close(fd);
    return false;
  }
  uint8_t *buf = (uint8_t *)malloc(len);
  ssize_t n = read(fd, buf, len);
  close(fd);
  if (n != (ssize_t)len) {
    free(buf);
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != fill) {
      free(buf);
      return false;
    }
  }
  free(buf);
  return true;
}

static void put_chunks(xTransferCache c, uint32_t chunk_size, uint32_t n_chunks,
                       uint8_t fill) {
  uint8_t *buf = (uint8_t *)malloc(chunk_size);
  memset(buf, fill, chunk_size);
  for (uint32_t i = 0; i < n_chunks; i++)
    xTransferCachePut(c, i, buf, chunk_size);
  free(buf);
}

/* ───────────────────── Lifecycle ───────────────────── */

TEST(xTransferCache, CreateDestroy) {
  size_t fsize = 128 * 1024;
  char *path = make_temp_file("xfer_cache_basic.bin", fsize);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, fsize, 4096, 4, 0);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(xTransferCachePendingBytes(c), 0u);

  xTransferCacheDestroy(c);
  close(fd);
  unlink(path);
  free(path);
}

TEST(xTransferCache, DestroyNull) {
  xTransferCacheDestroy(NULL);
}

/* ───────────────────── Basic Put and Flush ───────────────────── */

TEST(xTransferCache, SequentialChunks) {
  const uint32_t chunk_size = 1024;
  const uint32_t n_chunks  = 8;
  const size_t   file_size = chunk_size * n_chunks;

  char *path = make_temp_file("xfer_cache_seq.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 4, 0);
  ASSERT_NE(c, nullptr);

  put_chunks(c, chunk_size, n_chunks, 0xAB);

  xTransferCacheFlush(c, true);
  xTransferCacheDestroy(c);
  close(fd);

  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, 0xAB));

  unlink(path);
  free(path);
}

/* ───────────────────── Batch Flush ───────────────────── */

TEST(xTransferCache, BatchFlushTriggered) {
  const uint32_t chunk_size = 512;
  const uint32_t batch      = 4;
  const uint32_t n_chunks  = 8;
  const size_t   file_size = chunk_size * n_chunks;

  char *path = make_temp_file("xfer_cache_batch.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, batch, 0);
  ASSERT_NE(c, nullptr);

  put_chunks(c, chunk_size, n_chunks, 0xCD);

  /* Auto-flush should have fired at chunks 4 and 8. */
  xTransferCacheFlush(c, true);
  xTransferCacheDestroy(c);
  close(fd);

  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, 0xCD));

  unlink(path);
  free(path);
}

/* ───────────────────── Small File (No mmap) ───────────────────── */

TEST(xTransferCache, SmallFileNoMmap) {
  /* File smaller than XFER_TRANSFER_CACHE_MIN_FILE_SIZE: writes bypass mmap. */
  const uint32_t chunk_size = 512;
  const uint32_t n_chunks  = 4;
  const size_t   file_size = chunk_size * n_chunks; /* 2 KB */

  ASSERT_LT(file_size, (size_t)XFER_TRANSFER_CACHE_MIN_FILE_SIZE);

  char *path = make_temp_file("xfer_cache_small.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 2, 0);
  ASSERT_NE(c, nullptr);

  put_chunks(c, chunk_size, n_chunks, 0xEF);

  /* Small file: writes go directly to fd, no pending bytes. */
  EXPECT_EQ(xTransferCachePendingBytes(c), 0u);

  xTransferCacheDestroy(c);
  close(fd);

  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, 0xEF));

  unlink(path);
  free(path);
}

/* ───────────────────── Chunk Size Overflow ───────────────────── */

TEST(xTransferCache, ChunkSizeOverflow) {
  size_t   fsize     = 128 * 1024;
  char    *path      = make_temp_file("xfer_cache_ovf.bin", fsize);
  ASSERT_NE(path, nullptr);

  int      fd = open(path, O_RDWR);
  xTransferCache c = xTransferCacheCreate(fd, fsize, 4096, 4, 0);
  ASSERT_NE(c, nullptr);

  uint8_t buf[8192];
  EXPECT_EQ(xTransferCachePut(c, 0, buf, sizeof(buf)), xErrno_InvalidArg);

  xTransferCacheDestroy(c);
  close(fd);
  unlink(path);
  free(path);
}

/* ───────────────────── Sync vs Async Flush ───────────────────── */

TEST(xTransferCache, SyncFlush) {
  const uint32_t chunk_size = 1024;
  const uint32_t n_chunks  = 4;
  const size_t   file_size = chunk_size * n_chunks;

  char *path = make_temp_file("xfer_cache_sync.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 100, 0);
  ASSERT_NE(c, nullptr);

  put_chunks(c, chunk_size, n_chunks, 0x12);

  xTransferCacheFlush(c, true); /* MS_SYNC: returns only after data on disk */
  xTransferCacheDestroy(c);
  close(fd);

  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, 0x12));

  unlink(path);
  free(path);
}

/* ───────────────────── Destroy Flushes Pending ───────────────────── */

TEST(xTransferCache, DestroyFlushesPending) {
  const uint32_t chunk_size = 1024;
  const uint32_t n_chunks  = 4;
  const size_t   file_size = chunk_size * n_chunks;

  char *path = make_temp_file("xfer_cache_destroy.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 100, 0);
  ASSERT_NE(c, nullptr);

  put_chunks(c, chunk_size, n_chunks, 0x34);

  /* Destroy without explicit flush: must still persist data. */
  xTransferCacheDestroy(c);
  close(fd);

  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, 0x34));

  unlink(path);
  free(path);
}

/* ───────────────────── Out-of-Order Chunks ───────────────────── */

TEST(xTransferCache, OutOfOrderChunks) {
  const uint32_t chunk_size = 512;
  const uint32_t n_chunks  = 8;
  const size_t   file_size = chunk_size * n_chunks;

  char *path = make_temp_file("xfer_cache_oos.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 4, 0);
  ASSERT_NE(c, nullptr);

  uint8_t *buf = (uint8_t *)malloc(chunk_size);

  /* Write in reverse order. */
  for (uint32_t i = n_chunks; i-- > 0; ) {
    memset(buf, (uint8_t)(i + 1), chunk_size);
    ASSERT_EQ(xTransferCachePut(c, i, buf, chunk_size), xErrno_Ok);
  }

  free(buf);

  xTransferCacheFlush(c, true);
  xTransferCacheDestroy(c);
  close(fd);

  /* Verify each chunk has the correct fill byte. */
  for (uint32_t i = 0; i < n_chunks; i++)
    EXPECT_TRUE(verify_chunk(path, (uint64_t)i * chunk_size, chunk_size, (uint8_t)(i + 1)));

  unlink(path);
  free(path);
}

/* ───────────────────── Partial Chunk ───────────────────── */

TEST(xTransferCache, PartialChunk) {
  const uint32_t chunk_size = 4096;
  const size_t   file_size = chunk_size;

  char *path = make_temp_file("xfer_cache_partial.bin", file_size);
  ASSERT_NE(path, nullptr);

  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);

  xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size, 1, 0);
  ASSERT_NE(c, nullptr);

  /* Write less than a full chunk. */
  uint8_t buf[1024];
  memset(buf, 0x55, sizeof(buf));
  ASSERT_EQ(xTransferCachePut(c, 0, buf, 1024), xErrno_Ok);

  xTransferCacheFlush(c, true);
  xTransferCacheDestroy(c);
  close(fd);

  EXPECT_TRUE(verify_chunk(path, 0, 1024, 0x55));
  /* Bytes beyond the partial write are undefined (sparse/hole). */

  unlink(path);
  free(path);
}

/* ───────────────────── Null Safety ───────────────────── */

TEST(xTransferCache, PutNullCache) {
  EXPECT_EQ(xTransferCachePut(NULL, 0, "data", 4), xErrno_InvalidArg);
}

TEST(xTransferCache, FlushNullCache) {
  xTransferCacheFlush(NULL, true);
}

TEST(xTransferCache, PendingBytesNullCache) {
  EXPECT_EQ(xTransferCachePendingBytes(NULL), 0u);
}
