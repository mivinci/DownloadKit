/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_sender.c - Sender-side logic for P2P file transfer
 *
 * Handles file chunking, flow control (backpressure), resume bitmap
 * processing, and FILE_META / FILE_DONE / FILE_ACK messaging.
 */

#include "xfer_private.h"

#include <x/base/log.h>

#include <stdlib.h>
#include <string.h>

/* ── Forward declaration ───────────────────────────────── */

static void sender_schedule_next(void *arg);

/* ── Send next chunk ───────────────────────────────────── */

/**
 * @brief Send one chunk and schedule the next via a 0ms timer.
 *
 * Yielding after each chunk allows the event loop to process incoming
 * SCTP data (e.g. from the receiver in a loopback scenario). A
 * synchronous loop would starve the receiver.
 *
 * When paused due to backpressure, the on_buffered_amount_low
 * callback will resume sending.
 */
static void sender_send_next_chunk(xTransfer_ *impl) {
  /* Skip chunks that the receiver already has (resume). */
  while (impl->send_next_chunk < impl->send_total_chunks &&
         impl->send_has_resume &&
         xBitmapTest(&impl->send_resume_bitmap, impl->send_next_chunk)) {
    impl->send_next_chunk++;
  }

  /* All chunks sent? */
  if (impl->send_next_chunk >= impl->send_total_chunks) {
    uint8_t buf[64];
    size_t  len = 0;

    /* Compute SHA-1 over the entire source file */
    if (xfer_compute_file_sha1(impl->vfs, impl->send_filepath,
                               impl->send_sha1) != xErrno_Ok) {
      xfer_report_error(impl, xErrno_SysError,
                        "Failed to compute file SHA-1");
      return;
    }

    xTransferFileDone done;
    done.total_chunks = impl->send_total_chunks;
    memcpy(done.sha1, impl->send_sha1, XFER_SHA1_SIZE);

    if (xTransferEncodeFileDone(&done, buf, sizeof(buf), &len) != xErrno_Ok) {
      xfer_report_error(impl, xErrno_Unknown, "Failed to encode FILE_DONE");
      return;
    }

    xDataChannelSendBinary(impl->dc, buf, len);

    /* Wait for FILE_ACK from receiver before declaring Done.
       The receiver will verify SHA-1 and reply with an ACK. */
    impl->send_waiting_ack = true;
    XDEBUG("[xfer] sender: FILE_DONE sent, waiting for FILE_ACK");
    return;
  }

  /* Check backpressure: pause if buffered amount is too high */
  size_t buffered = xDataChannelGetBufferedAmount(impl->dc);
  if (buffered > XFER_SEND_HIGH_WATER_MARK) {
    impl->send_paused = true;
    xDataChannelSetBufferedAmountLowThreshold(impl->dc,
                                              XFER_SEND_LOW_WATER_MARK);
    XDEBUG("[xfer] sender paused: buffered=%zu high_water=%d",
           buffered, XFER_SEND_HIGH_WATER_MARK);
    return;
  }

  /* Read chunk from file via VFS */
  uint64_t offset = (uint64_t)impl->send_next_chunk * impl->send_chunk_size;
  uint8_t *buf = (uint8_t *)malloc(5 + impl->send_chunk_size);
  if (!buf) {
    xfer_report_error(impl, xErrno_NoMemory,
                      "Failed to allocate chunk buffer");
    return;
  }

  /* Encode chunk header */
  size_t hdr_len = 0;
  xTransferEncodeChunkHeader(impl->send_next_chunk, buf, 5, &hdr_len);

  /* Read file data */
  size_t nread = 0;
  xErrno read_err = impl->vfs->pread(impl->vfs->ctx, impl->send_handle,
                                     buf + hdr_len, impl->send_chunk_size,
                                     offset, &nread);
  if (read_err != xErrno_Ok || (nread == 0 && offset < impl->send_filesize)) {
    xfer_report_error(impl, xErrno_SysError, "Failed to read file");
    free(buf);
    return;
  }

  /* Send */
  xErrno err = xDataChannelSendBinary(impl->dc, buf, hdr_len + nread);
  free(buf);

  if (err == xErrno_Again) {
    /* SCTP send buffer full (EAGAIN): pause and retry later */
    impl->send_paused = true;
    xDataChannelSetBufferedAmountLowThreshold(impl->dc,
                                              XFER_SEND_LOW_WATER_MARK);
    XDEBUG("[xfer] sender paused: SCTP EAGAIN on chunk %u",
           impl->send_next_chunk);
    return;
  }
  if (err != xErrno_Ok) {
    xfer_report_error(impl, err, "Failed to send chunk");
    return;
  }

  impl->send_next_chunk++;
  uint64_t transferred =
    (uint64_t)(impl->send_next_chunk - 1) * impl->send_chunk_size + nread;
  if (transferred > impl->send_filesize) transferred = impl->send_filesize;
  xfer_report_progress(impl, transferred, impl->send_filesize);

  /* Yield: schedule next chunk via 0ms timer */
  xEventLoopTimerAfter(impl->loop, sender_schedule_next, impl, 0);
}

static void sender_schedule_next(void *arg) {
  xTransfer_ *impl = (xTransfer_ *)arg;
  sender_send_next_chunk(impl);
}

/* ── DataChannel: buffered amount low ──────────────────── */

/**
 * @brief Called when the DataChannel's buffered amount drops below
 *        the low-water threshold. Resumes sending if paused.
 */
void sender_on_buffered_amount_low(xDataChannel channel, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)channel;

  if (!impl->send_paused) return;

  XDEBUG("[xfer] sender resumed: buffered amount low");
  impl->send_paused = false;
  sender_send_next_chunk(impl);
}

/* ── DataChannel: message handler ──────────────────────── */

/**
 * @brief Sender-side message handler. Receives FILE_ACK / FILE_RESUME
 *        from receiver.
 */
void sender_on_dc_message(xDataChannel channel, xDataChannelMsgType type,
                          const uint8_t *data, size_t len, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)channel;
  (void)type;

  if (len < 1) return;

  uint8_t msg_type = data[0];
  const uint8_t *payload = data + 1;
  size_t payload_len = len - 1;

  switch (msg_type) {
  case XFER_MSG_ACK: {
    if (!impl->send_waiting_ack) {
      XDEBUG("[xfer] sender: unexpected FILE_ACK, ignoring");
      break;
    }
    impl->send_waiting_ack = false;

    xTransferFileAck ack;
    if (xTransferDecodeFileAck(payload, payload_len, &ack) != xErrno_Ok) {
      xfer_report_error(impl, xErrno_InvalidArg, "Invalid FILE_ACK");
      return;
    }

    if (ack.status == 0) {
      xfer_set_state(impl, xTransferState_Done);
    } else {
      xfer_report_error(impl, xErrno_Unknown,
                        "Receiver reported verification failure");
    }
    break;
  }

  case XFER_MSG_FILE_RESUME: {
    xTransferFileResume resume;
    if (xTransferDecodeFileResume(payload, payload_len, &resume) !=
        xErrno_Ok) {
      xfer_report_error(impl, xErrno_InvalidArg, "Invalid FILE_RESUME");
      return;
    }

    /* Validate total_chunks matches */
    if (resume.total_chunks != impl->send_total_chunks) {
      xfer_report_error(impl, xErrno_InvalidArg,
                        "FILE_RESUME total_chunks mismatch");
      return;
    }

    /* Load bitmap from the received data */
    if (resume.bitmap && resume.bitmap_len > 0) {
      xErrno err = xBitmapInit(&impl->send_resume_bitmap,
                               resume.total_chunks);
      if (err != xErrno_Ok) {
        xfer_report_error(impl, err, "Failed to init resume bitmap");
        return;
      }
      /* Copy bitmap data */
      uint32_t copy_len = resume.bitmap_len;
      if (copy_len > impl->send_resume_bitmap.nbytes)
        copy_len = impl->send_resume_bitmap.nbytes;
      memcpy(impl->send_resume_bitmap.data, resume.bitmap, copy_len);
      impl->send_has_resume = true;

      uint32_t already_done = xBitmapCount(&impl->send_resume_bitmap);
      (void)already_done;
      XDEBUG("[xfer] sender: resume bitmap received, %u/%u chunks done",
             already_done, resume.total_chunks);
    }

    /* Now start sending (only missing chunks) */
    if (impl->send_waiting_resume) {
      impl->send_waiting_resume = false;
      sender_send_next_chunk(impl);
    }
    break;
  }

  default:
    XDEBUG("[xfer] sender: ignoring message type 0x%02x", msg_type);
    break;
  }
}

/* ── DataChannel: open → send FILE_META then wait for resume ─ */

/**
 * @brief Called when the sender's DataChannel opens.
 *
 * Sends FILE_META to the receiver and waits for FILE_RESUME before
 * starting to send chunks.
 */
void sender_on_dc_open(xDataChannel channel, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;

  /* When the DataChannel was queued (pending), impl->dc may be NULL.
     Update it now that the channel is actually open. */
  impl->dc = channel;

  xfer_set_state(impl, xTransferState_Transferring);

  /* Send FILE_META */
  xTransferFileMeta meta;
  memset(&meta, 0, sizeof(meta));
  size_t name_len = strlen(impl->send_filename);
  if (name_len > 255) name_len = 255;
  memcpy(meta.filename, impl->send_filename, name_len);
  meta.filename_len = (uint16_t)name_len;
  meta.file_size = impl->send_filesize;
  meta.chunk_size = impl->send_chunk_size;
  memcpy(meta.sha1, impl->send_sha1, XFER_SHA1_SIZE);

  uint8_t buf[512];
  size_t  len = 0;
  if (xTransferEncodeFileMeta(&meta, buf, sizeof(buf), &len) != xErrno_Ok) {
    xfer_report_error(impl, xErrno_Unknown, "Failed to encode FILE_META");
    return;
  }

  xDataChannelSendBinary(impl->dc, buf, len);

  /* Wait for FILE_RESUME from receiver before sending chunks.
     The receiver will inspect its local state and reply with a bitmap
     indicating which chunks it already has. */
  impl->send_waiting_resume = true;
}
