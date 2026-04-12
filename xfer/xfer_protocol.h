/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_protocol.h - Wire protocol for xfer file transfer
 *
 * Defines the binary message format used over WebRTC DataChannel
 * for file metadata exchange, chunked data transfer, and completion
 * signaling.
 *
 * All multi-byte integers are in network byte order (big-endian).
 */

#ifndef XFER_PROTOCOL_H
#define XFER_PROTOCOL_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdint.h>

/* ───────────────────── Message Types ───────────────────── */

#define XFER_MSG_FILE_META 0x01 /**< File metadata (name, size, hash). */
#define XFER_MSG_FILE_CHUNK 0x02 /**< File data chunk.                 */
#define XFER_MSG_FILE_DONE 0x03 /**< Transfer complete.                */
#define XFER_MSG_ACK 0x04       /**< Acknowledgement.                  */
#define XFER_MSG_ERROR 0x05     /**< Error message.                    */
#define XFER_MSG_CANCEL 0x06    /**< Cancel transfer.                  */
#define XFER_MSG_FILE_RESUME 0x07 /**< Resume bitmap (receiver → sender). */

/* ───────────────────── SHA-1 ───────────────────── */

#define XFER_SHA1_SIZE 20

/* ───────────────────── Message Header ───────────────────── */

/**
 * Common header for all xfer messages (1 byte).
 *
 *   +------+
 *   | type |  1 byte
 *   +------+
 */

/* ───────────────────── FILE_META ───────────────────── */

/**
 * FILE_META message layout:
 *
 *   +------+----------+-----------+----------+----------+--------+
 *   | type | name_len |   name    |  size    | chunk_sz |  sha1  |
 *   | 1B   |   2B     | name_len  |   8B     |   4B     |  20B   |
 *   +------+----------+-----------+----------+----------+--------+
 */
XDEF_STRUCT(xTransferFileMeta) {
  char     filename[256]; /**< Original filename (no path).  */
  uint16_t filename_len;  /**< Length of filename.            */
  uint64_t file_size;     /**< Total file size in bytes.      */
  uint32_t chunk_size;    /**< Chunk size in bytes.            */
  uint8_t  sha1[XFER_SHA1_SIZE]; /**< SHA-1 of the file. */
};

/* ───────────────────── FILE_CHUNK ───────────────────── */

/**
 * FILE_CHUNK message layout:
 *
 *   +------+----------+----------+
 *   | type | chunk_id |   data   |
 *   | 1B   |   4B     | variable |
 *   +------+----------+----------+
 */
XDEF_STRUCT(xTransferFileChunk) {
  uint32_t       chunk_id; /**< Zero-based chunk index.       */
  const uint8_t *data;     /**< Chunk data (not owned).        */
  uint32_t       data_len; /**< Length of chunk data.           */
};

/* ───────────────────── FILE_DONE ───────────────────── */

/**
 * FILE_DONE message layout:
 *
 *   +------+-----------+--------+
 *   | type | total_chk |  sha1  |
 *   | 1B   |    4B     |  20B   |
 *   +------+-----------+--------+
 */
XDEF_STRUCT(xTransferFileDone) {
  uint32_t total_chunks;             /**< Total number of chunks sent. */
  uint8_t  sha1[XFER_SHA1_SIZE]; /**< SHA-1 of the entire file.  */
};

/* ───────────────────── FILE_ACK ───────────────────── */

/**
 * FILE_ACK message layout (sent by receiver after FILE_DONE):
 *
 *   +------+--------+
 *   | type | status |
 *   | 1B   |   1B   |
 *   +------+--------+
 *
 * status: 0 = success, non-zero = failure.
 */
XDEF_STRUCT(xTransferFileAck) {
  uint8_t status; /**< 0 = success, non-zero = failure. */
};

/* ───────────────────── FILE_RESUME ───────────────────── */

/**
 * FILE_RESUME message layout (sent by receiver after FILE_META):
 *
 *   +------+-----------+------------+------------------+
 *   | type | total_chk | bitmap_len |     bitmap[]     |
 *   | 1B   |    4B     |    4B      | bitmap_len bytes |
 *   +------+-----------+------------+------------------+
 *
 * The bitmap is a bitfield where bit i (LSB of byte i/8) indicates
 * whether chunk i has already been received. The sender uses this
 * to skip completed chunks (resume /断点续传).
 */
XDEF_STRUCT(xTransferFileResume) {
  uint32_t       total_chunks; /**< Total number of chunks.             */
  const uint8_t *bitmap;       /**< Bitmap data (not owned).            */
  uint32_t       bitmap_len;   /**< Length of bitmap in bytes.          */
};

/* ───────────────────── Encode / Decode ───────────────────── */

/**
 * @brief Encode a FILE_META message into a buffer.
 *
 * @param meta  File metadata.
 * @param buf   Output buffer (must be large enough).
 * @param cap   Buffer capacity.
 * @param out   Actual encoded length (output).
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferEncodeFileMeta(const xTransferFileMeta *meta,
                                       uint8_t *buf, size_t cap,
                                       size_t *out);

/**
 * @brief Decode a FILE_META message from a buffer.
 *
 * @param data  Input buffer (starting after the type byte).
 * @param len   Length of input.
 * @param meta  Output metadata.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferDecodeFileMeta(const uint8_t *data, size_t len,
                                       xTransferFileMeta *meta);

/**
 * @brief Encode a FILE_CHUNK header into a buffer.
 *
 * Only encodes the header (type + chunk_id). The caller appends
 * the chunk data after the header.
 *
 * @param chunk_id  Chunk index.
 * @param buf       Output buffer.
 * @param cap       Buffer capacity.
 * @param out       Actual header length (output).
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferEncodeChunkHeader(uint32_t chunk_id, uint8_t *buf,
                                          size_t cap, size_t *out);

/**
 * @brief Decode a FILE_CHUNK header from a buffer.
 *
 * @param data      Input buffer (starting after the type byte).
 * @param len       Length of input.
 * @param chunk_id  Output chunk index.
 * @param payload   Output pointer to chunk data.
 * @param payload_len Output length of chunk data.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferDecodeChunkHeader(const uint8_t *data, size_t len,
                                          uint32_t *chunk_id,
                                          const uint8_t **payload,
                                          uint32_t *payload_len);

/**
 * @brief Encode a FILE_DONE message into a buffer.
 *
 * @param done  Done metadata.
 * @param buf   Output buffer.
 * @param cap   Buffer capacity.
 * @param out   Actual encoded length (output).
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferEncodeFileDone(const xTransferFileDone *done,
                                       uint8_t *buf, size_t cap,
                                       size_t *out);

/**
 * @brief Decode a FILE_DONE message from a buffer.
 *
 * @param data  Input buffer (starting after the type byte).
 * @param len   Length of input.
 * @param done  Output done metadata.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferDecodeFileDone(const uint8_t *data, size_t len,
                                       xTransferFileDone *done);

/**
 * @brief Encode a FILE_RESUME message into a buffer.
 *
 * @param resume  Resume metadata (total_chunks + bitmap).
 * @param buf     Output buffer.
 * @param cap     Buffer capacity.
 * @param out     Actual encoded length (output).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferEncodeFileResume(const xTransferFileResume *resume,
                                         uint8_t *buf, size_t cap,
                                         size_t *out);

/**
 * @brief Decode a FILE_RESUME message from a buffer.
 *
 * @param data    Input buffer (starting after the type byte).
 * @param len     Length of input.
 * @param resume  Output resume metadata. The bitmap pointer points
 *                into the input buffer (zero-copy).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferDecodeFileResume(const uint8_t *data, size_t len,
                                         xTransferFileResume *resume);

/**
 * @brief Encode a FILE_ACK message into a buffer.
 *
 * @param ack   Ack metadata.
 * @param buf   Output buffer.
 * @param cap   Buffer capacity.
 * @param out   Actual encoded length (output).
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferEncodeFileAck(const xTransferFileAck *ack,
                                      uint8_t *buf, size_t cap,
                                      size_t *out);

/**
 * @brief Decode a FILE_ACK message from a buffer.
 *
 * @param data  Input buffer (starting after the type byte).
 * @param len   Length of input.
 * @param ack   Output ack metadata.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferDecodeFileAck(const uint8_t *data, size_t len,
                                      xTransferFileAck *ack);

#endif /* XFER_PROTOCOL_H */
