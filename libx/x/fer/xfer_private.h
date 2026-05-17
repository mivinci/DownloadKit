/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_private.h - Internal definitions shared across xfer source files
 *
 * This header is NOT part of the public API.  It exposes the opaque
 * xTransfer_ struct and helper functions so that xfer_sender.c and
 * xfer_receiver.c can access them.
 */

#ifndef XFER_PRIVATE_H
#define XFER_PRIVATE_H

#include "xfer.h"
#include "xfer_protocol.h"
#include "xfer_signal.h"

#include <x/base/bitmap.h>
#include <x/base/event.h>
#include <x/p2p/peer_connection.h>

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

/**
 * High-water mark for the DataChannel send buffer.
 * When buffered amount exceeds this, we pause sending and wait for
 * the on_buffered_amount_low callback to resume.
 */
#define XFER_SEND_HIGH_WATER_MARK (256 * 1024) /* 256 KB */

/**
 * Low-water mark: resume sending when buffered amount drops to this.
 */
#define XFER_SEND_LOW_WATER_MARK (64 * 1024) /* 64 KB */

/**
 * Bitmap persistence interval: persist the receiver bitmap every N chunks
 * instead of every single chunk, to reduce disk I/O overhead.
 * On resume, at most N chunks worth of progress may be lost.
 */
#define XFER_BITMAP_PERSIST_INTERVAL 32

/* ───────────────────── Internal State ───────────────────── */

XDEF_STRUCT(xTransfer_) {
  xEventLoop          loop;
  xTransferConf       conf;
  xTransferState      state;
  xTransferRole       role;
  xPeerConnection     pc;
  xDataChannel        dc;
  const xTransferVfs *vfs; /**< Active VFS (never NULL). */

  /* Sender state */
  void    *send_handle; /**< VFS handle for source file. */
  char     send_filepath[512];
  char     send_filename[XFER_MAX_FILENAME_LEN];
  uint64_t send_filesize;
  uint32_t send_chunk_size;
  uint32_t send_total_chunks;
  uint32_t send_next_chunk;
  uint8_t  send_sha1[XFER_SHA1_SIZE];
  xBitmap  send_resume_bitmap;  /**< Bitmap from receiver (resume). */
  bool     send_has_resume;     /**< True if FILE_RESUME received.  */
  bool     send_waiting_resume; /**< True if waiting for FILE_RESUME before sending chunks. */

  /* Receiver state */
  void    *recv_handle; /**< VFS handle for destination file. */
  char     recv_dest_dir[512];
  char     recv_filepath[1024]; /**< Full path to the .part file.   */
  char     recv_filename[XFER_MAX_FILENAME_LEN];
  uint64_t recv_filesize;
  uint32_t recv_chunk_size;
  uint32_t recv_total_chunks;
  uint32_t recv_chunks_received;
  uint64_t recv_bytes_received;
  uint8_t  recv_sha1[XFER_SHA1_SIZE];
  xBitmap  recv_bitmap;            /**< Tracks which chunks received.  */
  char     recv_bitmap_path[1024]; /**< Path to .bitmap file.       */

  /* Code for signaling */
  char code[XFER_MAX_CODE_LEN];

  /* Signaling client (NULL when using manual SDP API) */
  xSignalClient signal;

  /* Flow control: true when paused due to backpressure */
  bool send_paused;

  /* True after FILE_DONE is sent; waiting for FILE_ACK from receiver */
  bool send_waiting_ack;
};

/* ── Shared helpers (defined in xfer.c) ────────────────── */

void   xfer_set_state(xTransfer_ *impl, xTransferState state);
void   xfer_report_error(xTransfer_ *impl, xErrno err, const char *msg);
void   xfer_report_progress(xTransfer_ *impl, uint64_t transferred, uint64_t total);
xErrno xfer_compute_file_sha1(const xTransferVfs *vfs, const char *path, uint8_t *digest);

/* ── Bitmap persistence helpers (defined in xfer.c) ────── */

xErrno xfer_bitmap_save(const xBitmap *bm, uint32_t total_chunks, const char *path);
xErrno xfer_bitmap_load(xBitmap *bm, uint32_t *total_chunks, const char *path);

/* ── Sender callbacks (defined in xfer_sender.c) ──────── */

void sender_on_dc_open(xDataChannel channel, void *ctx);
void sender_on_dc_message(xDataChannel channel, xDataChannelMsgType type, const uint8_t *data,
                          size_t len, void *ctx);
void sender_on_buffered_amount_low(xDataChannel channel, void *ctx);

/* ── Receiver callbacks (defined in xfer_receiver.c) ──── */

void receiver_on_dc_message(xDataChannel channel, xDataChannelMsgType type, const uint8_t *data,
                            size_t len, void *ctx);

#endif /* XFER_PRIVATE_H */
