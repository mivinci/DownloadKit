/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_protocol_test.cpp - Wire protocol encode/decode tests
 */

#include <gtest/gtest.h>

extern "C" {
#include "xfer_protocol.h"
}

#include <cstring>
#include <vector>

/* ───────────────────── FILE_META ───────────────────── */

TEST(XferProtocol, EncodeDecodeFileMeta) {
  xTransferFileMeta meta;
  memset(&meta, 0, sizeof(meta));
  strcpy(meta.filename, "test_video.mp4");
  meta.filename_len = (uint16_t)strlen(meta.filename);
  meta.file_size = 1024 * 1024 * 100ULL; /* 100 MB */
  meta.chunk_size = 64 * 1024;
  memset(meta.sha1, 0xAB, XFER_SHA1_SIZE);

  uint8_t buf[512];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileMeta(&meta, buf, sizeof(buf), &len), xErrno_Ok);
  ASSERT_GT(len, 0u);

  /* First byte should be message type */
  ASSERT_EQ(buf[0], XFER_MSG_FILE_META);

  /* Decode (skip type byte) */
  xTransferFileMeta decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(xTransferDecodeFileMeta(buf + 1, len - 1, &decoded), xErrno_Ok);

  EXPECT_STREQ(decoded.filename, "test_video.mp4");
  EXPECT_EQ(decoded.filename_len, meta.filename_len);
  EXPECT_EQ(decoded.file_size, meta.file_size);
  EXPECT_EQ(decoded.chunk_size, meta.chunk_size);
  EXPECT_EQ(memcmp(decoded.sha1, meta.sha1, XFER_SHA1_SIZE), 0);
}

TEST(XferProtocol, FileMetaBufferTooSmall) {
  xTransferFileMeta meta;
  memset(&meta, 0, sizeof(meta));
  strcpy(meta.filename, "a.txt");
  meta.filename_len = 5;
  meta.file_size = 100;
  meta.chunk_size = 100;

  uint8_t buf[10]; /* Too small */
  size_t  len = 0;
  EXPECT_EQ(xTransferEncodeFileMeta(&meta, buf, sizeof(buf), &len),
            xErrno_InvalidArg);
}

TEST(XferProtocol, FileMetaNullArgs) {
  uint8_t buf[512];
  size_t  len = 0;
  EXPECT_EQ(xTransferEncodeFileMeta(NULL, buf, sizeof(buf), &len),
            xErrno_InvalidArg);

  xTransferFileMeta meta;
  EXPECT_EQ(xTransferDecodeFileMeta(NULL, 0, &meta), xErrno_InvalidArg);
}

/* ───────────────────── FILE_CHUNK ───────────────────── */

TEST(XferProtocol, EncodeDecodeChunkHeader) {
  uint8_t buf[16];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeChunkHeader(42, buf, sizeof(buf), &len), xErrno_Ok);
  ASSERT_EQ(len, 5u);
  ASSERT_EQ(buf[0], XFER_MSG_FILE_CHUNK);

  /* Simulate a full chunk message: header + payload */
  uint8_t msg[16];
  memcpy(msg, buf, len);
  msg[5] = 0xDE;
  msg[6] = 0xAD;

  /* Decode (skip type byte) */
  uint32_t       chunk_id;
  const uint8_t *payload;
  uint32_t       payload_len;
  ASSERT_EQ(xTransferDecodeChunkHeader(msg + 1, 6, &chunk_id, &payload,
                                       &payload_len),
            xErrno_Ok);
  EXPECT_EQ(chunk_id, 42u);
  EXPECT_EQ(payload_len, 2u);
  EXPECT_EQ(payload[0], 0xDE);
  EXPECT_EQ(payload[1], 0xAD);
}

/* ───────────────────── FILE_DONE ───────────────────── */

TEST(XferProtocol, EncodeDecodeFileDone) {
  xTransferFileDone done;
  done.total_chunks = 1600;
  memset(done.sha1, 0xCD, XFER_SHA1_SIZE);

  uint8_t buf[64];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileDone(&done, buf, sizeof(buf), &len), xErrno_Ok);
  ASSERT_EQ(len, 1u + 4u + XFER_SHA1_SIZE);
  ASSERT_EQ(buf[0], XFER_MSG_FILE_DONE);

  /* Decode (skip type byte) */
  xTransferFileDone decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(xTransferDecodeFileDone(buf + 1, len - 1, &decoded), xErrno_Ok);

  EXPECT_EQ(decoded.total_chunks, 1600u);
  EXPECT_EQ(memcmp(decoded.sha1, done.sha1, XFER_SHA1_SIZE), 0);
}

TEST(XferProtocol, FileDoneBufferTooSmall) {
  xTransferFileDone done;
  done.total_chunks = 1;
  memset(done.sha1, 0, XFER_SHA1_SIZE);

  uint8_t buf[4]; /* Too small */
  size_t  len = 0;
  EXPECT_EQ(xTransferEncodeFileDone(&done, buf, sizeof(buf), &len),
            xErrno_InvalidArg);
}

/* ───────────────────── Roundtrip with large filename ── */

TEST(XferProtocol, LargeFilename) {
  xTransferFileMeta meta;
  memset(&meta, 0, sizeof(meta));

  /* Fill with 255 chars */
  memset(meta.filename, 'x', 255);
  meta.filename[255] = '\0';
  meta.filename_len = 255;
  meta.file_size = 999999999ULL;
  meta.chunk_size = 32768;

  uint8_t buf[512];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileMeta(&meta, buf, sizeof(buf), &len), xErrno_Ok);

  xTransferFileMeta decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(xTransferDecodeFileMeta(buf + 1, len - 1, &decoded), xErrno_Ok);
  EXPECT_EQ(decoded.filename_len, 255);
  EXPECT_EQ(decoded.file_size, 999999999ULL);
}

/* ───────────────────── FILE_RESUME ───────────────────── */

TEST(XferProtocol, EncodeDecodeFileResume) {
  /* Simulate a bitmap for 100 chunks: 13 bytes */
  uint8_t bitmap[13];
  memset(bitmap, 0, sizeof(bitmap));
  /* Mark chunks 0, 7, 8, 99 as received */
  bitmap[0] |= (1 << 0); /* chunk 0 */
  bitmap[0] |= (1 << 7); /* chunk 7 */
  bitmap[1] |= (1 << 0); /* chunk 8 */
  bitmap[12] |= (1 << 3); /* chunk 99 */

  xTransferFileResume resume;
  resume.total_chunks = 100;
  resume.bitmap = bitmap;
  resume.bitmap_len = 13;

  uint8_t buf[64];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileResume(&resume, buf, sizeof(buf), &len),
            xErrno_Ok);
  ASSERT_EQ(len, 1u + 4u + 4u + 13u); /* type + total + bm_len + bitmap */
  ASSERT_EQ(buf[0], XFER_MSG_FILE_RESUME);

  /* Decode (skip type byte) */
  xTransferFileResume decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(xTransferDecodeFileResume(buf + 1, len - 1, &decoded), xErrno_Ok);

  EXPECT_EQ(decoded.total_chunks, 100u);
  EXPECT_EQ(decoded.bitmap_len, 13u);
  ASSERT_NE(decoded.bitmap, nullptr);
  EXPECT_EQ(memcmp(decoded.bitmap, bitmap, 13), 0);
}

TEST(XferProtocol, FileResumeEmptyBitmap) {
  /* All-zero bitmap (fresh transfer) */
  uint8_t bitmap[2] = {0, 0};

  xTransferFileResume resume;
  resume.total_chunks = 16;
  resume.bitmap = bitmap;
  resume.bitmap_len = 2;

  uint8_t buf[32];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileResume(&resume, buf, sizeof(buf), &len),
            xErrno_Ok);

  xTransferFileResume decoded;
  ASSERT_EQ(xTransferDecodeFileResume(buf + 1, len - 1, &decoded), xErrno_Ok);
  EXPECT_EQ(decoded.total_chunks, 16u);
  EXPECT_EQ(decoded.bitmap_len, 2u);
  EXPECT_EQ(decoded.bitmap[0], 0);
  EXPECT_EQ(decoded.bitmap[1], 0);
}

TEST(XferProtocol, FileResumeFullBitmap) {
  /* All chunks done */
  uint8_t bitmap[2] = {0xFF, 0xFF};

  xTransferFileResume resume;
  resume.total_chunks = 16;
  resume.bitmap = bitmap;
  resume.bitmap_len = 2;

  uint8_t buf[32];
  size_t  len = 0;
  ASSERT_EQ(xTransferEncodeFileResume(&resume, buf, sizeof(buf), &len),
            xErrno_Ok);

  xTransferFileResume decoded;
  ASSERT_EQ(xTransferDecodeFileResume(buf + 1, len - 1, &decoded), xErrno_Ok);
  EXPECT_EQ(decoded.total_chunks, 16u);
  EXPECT_EQ(decoded.bitmap[0], 0xFF);
  EXPECT_EQ(decoded.bitmap[1], 0xFF);
}

TEST(XferProtocol, FileResumeBufferTooSmall) {
  uint8_t bitmap[4] = {0};
  xTransferFileResume resume;
  resume.total_chunks = 32;
  resume.bitmap = bitmap;
  resume.bitmap_len = 4;

  uint8_t buf[8]; /* Too small: need 1+4+4+4 = 13 */
  size_t  len = 0;
  EXPECT_EQ(xTransferEncodeFileResume(&resume, buf, sizeof(buf), &len),
            xErrno_InvalidArg);
}

TEST(XferProtocol, FileResumeNullArgs) {
  uint8_t buf[64];
  size_t  len = 0;
  EXPECT_EQ(xTransferEncodeFileResume(NULL, buf, sizeof(buf), &len),
            xErrno_InvalidArg);

  xTransferFileResume resume;
  EXPECT_EQ(xTransferDecodeFileResume(NULL, 0, &resume), xErrno_InvalidArg);
}

TEST(XferProtocol, FileResumeDecodeTruncated) {
  /* Only 4 bytes — missing bitmap_len */
  uint8_t data[4] = {0, 0, 0, 10};
  xTransferFileResume resume;
  EXPECT_EQ(xTransferDecodeFileResume(data, 4, &resume), xErrno_InvalidArg);
}

TEST(XferProtocol, FileResumeLargeBitmap) {
  /* 1GB file, 64KB chunks → 16384 chunks → 2048 bytes bitmap */
  const uint32_t total_chunks = 16384;
  const uint32_t bitmap_len = (total_chunks + 7) / 8;
  std::vector<uint8_t> bitmap(bitmap_len, 0);

  /* Mark every other chunk as received */
  for (uint32_t i = 0; i < total_chunks; i += 2) {
    bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
  }

  xTransferFileResume resume;
  resume.total_chunks = total_chunks;
  resume.bitmap = bitmap.data();
  resume.bitmap_len = bitmap_len;

  std::vector<uint8_t> buf(9 + bitmap_len);
  size_t len = 0;
  ASSERT_EQ(xTransferEncodeFileResume(&resume, buf.data(), buf.size(), &len),
            xErrno_Ok);
  ASSERT_EQ(len, 9u + bitmap_len);

  xTransferFileResume decoded;
  ASSERT_EQ(xTransferDecodeFileResume(buf.data() + 1, len - 1, &decoded),
            xErrno_Ok);
  EXPECT_EQ(decoded.total_chunks, total_chunks);
  EXPECT_EQ(decoded.bitmap_len, bitmap_len);
  EXPECT_EQ(memcmp(decoded.bitmap, bitmap.data(), bitmap_len), 0);
}
