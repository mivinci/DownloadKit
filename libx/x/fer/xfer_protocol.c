/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_protocol.c - Wire protocol encode/decode implementation
 */

#include "xfer_protocol.h"

#include <arpa/inet.h>
#include <string.h>

/* ── Helpers for 64-bit network byte order ─────────────── */

static inline void put_u16(uint8_t *buf, uint16_t val) {
  uint16_t n = htons(val);
  memcpy(buf, &n, 2);
}

static inline uint16_t get_u16(const uint8_t *buf) {
  uint16_t n;
  memcpy(&n, buf, 2);
  return ntohs(n);
}

static inline void put_u32(uint8_t *buf, uint32_t val) {
  uint32_t n = htonl(val);
  memcpy(buf, &n, 4);
}

static inline uint32_t get_u32(const uint8_t *buf) {
  uint32_t n;
  memcpy(&n, buf, 4);
  return ntohl(n);
}

static inline void put_u64(uint8_t *buf, uint64_t val) {
  put_u32(buf, (uint32_t)(val >> 32));
  put_u32(buf + 4, (uint32_t)(val & 0xFFFFFFFF));
}

static inline uint64_t get_u64(const uint8_t *buf) {
  uint64_t hi = get_u32(buf);
  uint64_t lo = get_u32(buf + 4);
  return (hi << 32) | lo;
}

/* ───────────────────── FILE_META ───────────────────── */

/*
 * Layout: type(1) + name_len(2) + name(N) + size(8) + chunk_sz(4) + sha1(20)
 * Total:  1 + 2 + N + 8 + 4 + 20 = 35 + N
 */

xErrno xTransferEncodeFileMeta(const xTransferFileMeta *meta, uint8_t *buf, size_t cap,
                               size_t *out) {
  if (!meta || !buf || !out) return xErrno_InvalidArg;

  size_t needed = 1 + 2 + meta->filename_len + 8 + 4 + XFER_SHA1_SIZE;
  if (cap < needed) return xErrno_InvalidArg;

  uint8_t *p = buf;

  /* Type */
  *p++ = XFER_MSG_FILE_META;

  /* Filename length + filename */
  put_u16(p, meta->filename_len);
  p += 2;
  memcpy(p, meta->filename, meta->filename_len);
  p += meta->filename_len;

  /* File size */
  put_u64(p, meta->file_size);
  p += 8;

  /* Chunk size */
  put_u32(p, meta->chunk_size);
  p += 4;

  /* SHA-1 */
  memcpy(p, meta->sha1, XFER_SHA1_SIZE);
  p += XFER_SHA1_SIZE;

  *out = (size_t)(p - buf);
  return xErrno_Ok;
}

xErrno xTransferDecodeFileMeta(const uint8_t *data, size_t len, xTransferFileMeta *meta) {
  if (!data || !meta) return xErrno_InvalidArg;

  /* Minimum: name_len(2) + size(8) + chunk_sz(4) + sha1(20) = 34 */
  if (len < 34) return xErrno_InvalidArg;

  const uint8_t *p = data;

  /* Filename length */
  uint16_t name_len = get_u16(p);
  p += 2;

  if (name_len > 255 || len < (size_t)(2 + name_len + 8 + 4 + XFER_SHA1_SIZE))
    return xErrno_InvalidArg;

  /* Filename */
  memcpy(meta->filename, p, name_len);
  meta->filename[name_len] = '\0';
  meta->filename_len       = name_len;
  p += name_len;

  /* File size */
  meta->file_size = get_u64(p);
  p += 8;

  /* Chunk size */
  meta->chunk_size = get_u32(p);
  p += 4;

  /* SHA-1 */
  memcpy(meta->sha1, p, XFER_SHA1_SIZE);

  return xErrno_Ok;
}

/* ───────────────────── FILE_CHUNK ───────────────────── */

/*
 * Layout: type(1) + chunk_id(4) + data(N)
 * Header: 5 bytes
 */

xErrno xTransferEncodeChunkHeader(uint32_t chunk_id, uint8_t *buf, size_t cap, size_t *out) {
  if (!buf || !out) return xErrno_InvalidArg;
  if (cap < 5) return xErrno_InvalidArg;

  buf[0] = XFER_MSG_FILE_CHUNK;
  put_u32(buf + 1, chunk_id);
  *out = 5;
  return xErrno_Ok;
}

xErrno xTransferDecodeChunkHeader(const uint8_t *data, size_t len, uint32_t *chunk_id,
                                  const uint8_t **payload, uint32_t *payload_len) {
  if (!data || !chunk_id || !payload || !payload_len) return xErrno_InvalidArg;

  /* Minimum: chunk_id(4) */
  if (len < 4) return xErrno_InvalidArg;

  *chunk_id    = get_u32(data);
  *payload     = data + 4;
  *payload_len = (uint32_t)(len - 4);
  return xErrno_Ok;
}

/* ───────────────────── FILE_DONE ───────────────────── */

/*
 * Layout: type(1) + total_chunks(4) + sha1(20)
 * Total:  25 bytes
 */

xErrno xTransferEncodeFileDone(const xTransferFileDone *done, uint8_t *buf, size_t cap,
                               size_t *out) {
  if (!done || !buf || !out) return xErrno_InvalidArg;

  size_t needed = 1 + 4 + XFER_SHA1_SIZE;
  if (cap < needed) return xErrno_InvalidArg;

  buf[0] = XFER_MSG_FILE_DONE;
  put_u32(buf + 1, done->total_chunks);
  memcpy(buf + 5, done->sha1, XFER_SHA1_SIZE);

  *out = needed;
  return xErrno_Ok;
}

xErrno xTransferDecodeFileDone(const uint8_t *data, size_t len, xTransferFileDone *done) {
  if (!data || !done) return xErrno_InvalidArg;

  /* total_chunks(4) + sha1(20) = 24 */
  if (len < 24) return xErrno_InvalidArg;

  done->total_chunks = get_u32(data);
  memcpy(done->sha1, data + 4, XFER_SHA1_SIZE);

  return xErrno_Ok;
}

/* ───────────────────── FILE_RESUME ───────────────────── */

/*
 * Layout: type(1) + total_chunks(4) + bitmap_len(4) + bitmap[bitmap_len]
 * Total:  9 + bitmap_len
 */

xErrno xTransferEncodeFileResume(const xTransferFileResume *resume, uint8_t *buf, size_t cap,
                                 size_t *out) {
  if (!resume || !buf || !out) return xErrno_InvalidArg;
  if (!resume->bitmap && resume->bitmap_len > 0) return xErrno_InvalidArg;

  size_t needed = 1 + 4 + 4 + resume->bitmap_len;
  if (cap < needed) return xErrno_InvalidArg;

  uint8_t *p = buf;

  /* Type */
  *p++ = XFER_MSG_FILE_RESUME;

  /* Total chunks */
  put_u32(p, resume->total_chunks);
  p += 4;

  /* Bitmap length */
  put_u32(p, resume->bitmap_len);
  p += 4;

  /* Bitmap data */
  if (resume->bitmap_len > 0) {
    memcpy(p, resume->bitmap, resume->bitmap_len);
    p += resume->bitmap_len;
  }

  *out = (size_t)(p - buf);
  return xErrno_Ok;
}

xErrno xTransferDecodeFileResume(const uint8_t *data, size_t len, xTransferFileResume *resume) {
  if (!data || !resume) return xErrno_InvalidArg;

  /* Minimum: total_chunks(4) + bitmap_len(4) = 8 */
  if (len < 8) return xErrno_InvalidArg;

  const uint8_t *p = data;

  resume->total_chunks = get_u32(p);
  p += 4;

  resume->bitmap_len = get_u32(p);
  p += 4;

  if (len < (size_t)(8 + resume->bitmap_len)) return xErrno_InvalidArg;

  /* Zero-copy: point into the input buffer */
  resume->bitmap = (resume->bitmap_len > 0) ? p : NULL;

  return xErrno_Ok;
}

/* ───────────────────── FILE_ACK ───────────────────── */

/*
 * Layout: type(1) + status(1)
 * Total:  2 bytes
 */

xErrno xTransferEncodeFileAck(const xTransferFileAck *ack, uint8_t *buf, size_t cap, size_t *out) {
  if (!ack || !buf || !out) return xErrno_InvalidArg;

  size_t needed = 1 + 1;
  if (cap < needed) return xErrno_InvalidArg;

  buf[0] = XFER_MSG_ACK;
  buf[1] = ack->status;

  *out = needed;
  return xErrno_Ok;
}

xErrno xTransferDecodeFileAck(const uint8_t *data, size_t len, xTransferFileAck *ack) {
  if (!data || !ack) return xErrno_InvalidArg;

  /* status(1) = 1 */
  if (len < 1) return xErrno_InvalidArg;

  ack->status = data[0];

  return xErrno_Ok;
}
