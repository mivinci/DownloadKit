/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer.c - P2P file transfer core implementation
 *
 * Orchestrates PeerConnection setup, DataChannel creation, and file
 * chunking/reassembly for peer-to-peer file transfer.
 */

#include "xfer.h"
#include "xfer_protocol.h"
#include "xfer_signal.h"

#include <xbase/bitmap.h>
#include <xbase/log.h>
#include <xp2p/peer_connection.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define XFER_SEND_LOW_WATER_MARK  (64 * 1024)  /* 64 KB */

/* ───────────────────── Internal State ───────────────────── */

XDEF_STRUCT(xTransfer_) {
  xEventLoop      loop;
  xTransferConf   conf;
  xTransferState  state;
  xTransferRole   role;
  xPeerConnection pc;
  xDataChannel    dc;

  /* Sender state */
  FILE    *send_fp;
  char     send_filepath[512];
  char     send_filename[XFER_MAX_FILENAME_LEN];
  uint64_t send_filesize;
  uint32_t send_chunk_size;
  uint32_t send_total_chunks;
  uint32_t send_next_chunk;
  uint8_t  send_sha256[XFER_SHA256_SIZE];
  xBitmap  send_resume_bitmap;  /**< Bitmap from receiver (resume). */
  bool     send_has_resume;     /**< True if FILE_RESUME received.  */
  bool     send_waiting_resume; /**< True if waiting for FILE_RESUME before sending chunks. */

  /* Receiver state */
  FILE    *recv_fp;
  char     recv_dest_dir[512];
  char     recv_filepath[1024]; /**< Full path to the .part file.   */
  char     recv_filename[XFER_MAX_FILENAME_LEN];
  uint64_t recv_filesize;
  uint32_t recv_chunk_size;
  uint32_t recv_total_chunks;
  uint32_t recv_chunks_received;
  uint64_t recv_bytes_received;
  uint8_t  recv_sha256[XFER_SHA256_SIZE];
  xBitmap  recv_bitmap;         /**< Tracks which chunks received.  */
  char     recv_bitmap_path[1024]; /**< Path to .bitmap file.       */

  /* Code for signaling */
  char code[XFER_MAX_CODE_LEN];

  /* Signaling client (NULL when using manual SDP API) */
  xSignalClient signal;

  /* Flow control: true when paused due to backpressure */
  bool send_paused;
};

/* ── Helpers ───────────────────────────────────────────── */

static void set_state(xTransfer_ *impl, xTransferState state) {
  impl->state = state;
  if (impl->conf.on_state_change) {
    impl->conf.on_state_change((xTransfer)impl, state, impl->conf.ctx);
  }
}

static void report_error(xTransfer_ *impl, xErrno err, const char *msg) {
  if (impl->conf.on_error) {
    impl->conf.on_error((xTransfer)impl, err, msg, impl->conf.ctx);
  }
  set_state(impl, xTransferState_Failed);
}

static void report_progress(xTransfer_ *impl, uint64_t transferred,
                            uint64_t total) {
  if (impl->conf.on_progress) {
    impl->conf.on_progress((xTransfer)impl, transferred, total, impl->conf.ctx);
  }
}

/* Extract basename from a file path. */
static const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash) return slash + 1;
  return path;
}

/* ── Bitmap persistence helpers ────────────────────────── */

/**
 * @brief Save a bitmap to a file.
 *
 * File format: total_chunks(4 bytes, big-endian) + bitmap raw bytes.
 */
static xErrno bitmap_save(const xBitmap *bm, uint32_t total_chunks,
                           const char *path) {
  FILE *fp = fopen(path, "wb");
  if (!fp) return xErrno_SysError;

  /* Write total_chunks in big-endian */
  uint8_t hdr[4];
  hdr[0] = (uint8_t)(total_chunks >> 24);
  hdr[1] = (uint8_t)(total_chunks >> 16);
  hdr[2] = (uint8_t)(total_chunks >> 8);
  hdr[3] = (uint8_t)(total_chunks);
  if (fwrite(hdr, 1, 4, fp) != 4) { fclose(fp); return xErrno_SysError; }

  /* Write bitmap data */
  uint32_t nbytes = 0;
  const uint8_t *data = xBitmapData(bm, &nbytes);
  if (data && nbytes > 0) {
    if (fwrite(data, 1, nbytes, fp) != nbytes) {
      fclose(fp);
      return xErrno_SysError;
    }
  }

  fclose(fp);
  return xErrno_Ok;
}

/**
 * @brief Load a bitmap from a file.
 *
 * @param bm            [out] Bitmap to initialise.
 * @param total_chunks  [out] Total chunks stored in the file.
 * @param path          Path to the .bitmap file.
 * @return xErrno_Ok on success, xErrno_NotFound if file doesn't exist.
 */
static xErrno bitmap_load(xBitmap *bm, uint32_t *total_chunks,
                           const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return xErrno_NotFound;

  /* Read total_chunks */
  uint8_t hdr[4];
  if (fread(hdr, 1, 4, fp) != 4) { fclose(fp); return xErrno_SysError; }
  *total_chunks = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                  ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];

  if (*total_chunks == 0) { fclose(fp); return xErrno_InvalidArg; }

  /* Init bitmap */
  xErrno err = xBitmapInit(bm, *total_chunks);
  if (err != xErrno_Ok) { fclose(fp); return err; }

  /* Read bitmap data */
  uint32_t nbytes = bm->nbytes;
  if (fread(bm->data, 1, nbytes, fp) != nbytes) {
    xBitmapFree(bm);
    fclose(fp);
    return xErrno_SysError;
  }

  fclose(fp);
  return xErrno_Ok;
}

/* ── Sender: send next chunk ───────────────────────────── */

static void sender_schedule_next(void *arg);

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

    xTransferFileDone done;
    done.total_chunks = impl->send_total_chunks;
    memcpy(done.sha256, impl->send_sha256, XFER_SHA256_SIZE);

    if (xTransferEncodeFileDone(&done, buf, sizeof(buf), &len) != xErrno_Ok) {
      report_error(impl, xErrno_Unknown, "Failed to encode FILE_DONE");
      return;
    }

    xDataChannelSendBinary(impl->dc, buf, len);
    set_state(impl, xTransferState_Done);
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

  /* Seek to the correct file offset for this chunk */
  uint64_t offset = (uint64_t)impl->send_next_chunk * impl->send_chunk_size;
  if (fseek(impl->send_fp, (long)offset, SEEK_SET) != 0) {
    report_error(impl, xErrno_SysError, "Failed to seek in file");
    return;
  }

  /* Read chunk from file */
  uint8_t *buf = (uint8_t *)malloc(5 + impl->send_chunk_size);
  if (!buf) {
    report_error(impl, xErrno_NoMemory, "Failed to allocate chunk buffer");
    return;
  }

  /* Encode chunk header */
  size_t hdr_len = 0;
  xTransferEncodeChunkHeader(impl->send_next_chunk, buf, 5, &hdr_len);

  /* Read file data */
  size_t nread =
    fread(buf + hdr_len, 1, impl->send_chunk_size, impl->send_fp);
  if (nread == 0 && ferror(impl->send_fp)) {
    report_error(impl, xErrno_SysError, "Failed to read file");
    free(buf);
    return;
  }

  /* Send */
  xErrno err = xDataChannelSendBinary(impl->dc, buf, hdr_len + nread);
  free(buf);

  if (err != xErrno_Ok) {
    report_error(impl, err, "Failed to send chunk");
    return;
  }

  impl->send_next_chunk++;
  uint64_t transferred =
    (uint64_t)(impl->send_next_chunk - 1) * impl->send_chunk_size + nread;
  if (transferred > impl->send_filesize) transferred = impl->send_filesize;
  report_progress(impl, transferred, impl->send_filesize);

  /* Yield: schedule next chunk via 0ms timer */
  xEventLoopTimerAfter(impl->loop, sender_schedule_next, impl, 0);
}

static void sender_schedule_next(void *arg) {
  xTransfer_ *impl = (xTransfer_ *)arg;
  sender_send_next_chunk(impl);
}

/* ── Sender: DataChannel callbacks ─────────────────────── */

/**
 * @brief Called when the DataChannel's buffered amount drops below
 *        the low-water threshold. Resumes sending if paused.
 */
static void sender_on_buffered_amount_low(xDataChannel channel, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)channel;

  if (!impl->send_paused) return;

  XDEBUG("[xfer] sender resumed: buffered amount low");
  impl->send_paused = false;
  sender_send_next_chunk(impl);
}

/**
 * @brief Sender-side message handler. Receives FILE_RESUME from receiver.
 */
static void sender_on_dc_message(xDataChannel channel,
                                 xDataChannelMsgType type,
                                 const uint8_t *data, size_t len,
                                 void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)channel;
  (void)type;

  if (len < 1) return;

  uint8_t msg_type = data[0];
  const uint8_t *payload = data + 1;
  size_t payload_len = len - 1;

  switch (msg_type) {
  case XFER_MSG_FILE_RESUME: {
    xTransferFileResume resume;
    if (xTransferDecodeFileResume(payload, payload_len, &resume) !=
        xErrno_Ok) {
      report_error(impl, xErrno_InvalidArg, "Invalid FILE_RESUME");
      return;
    }

    /* Validate total_chunks matches */
    if (resume.total_chunks != impl->send_total_chunks) {
      report_error(impl, xErrno_InvalidArg,
                   "FILE_RESUME total_chunks mismatch");
      return;
    }

    /* Load bitmap from the received data */
    if (resume.bitmap && resume.bitmap_len > 0) {
      xErrno err = xBitmapInit(&impl->send_resume_bitmap,
                               resume.total_chunks);
      if (err != xErrno_Ok) {
        report_error(impl, err, "Failed to init resume bitmap");
        return;
      }
      /* Copy bitmap data */
      uint32_t copy_len = resume.bitmap_len;
      if (copy_len > impl->send_resume_bitmap.nbytes)
        copy_len = impl->send_resume_bitmap.nbytes;
      memcpy(impl->send_resume_bitmap.data, resume.bitmap, copy_len);
      impl->send_has_resume = true;

      uint32_t already_done = xBitmapCount(&impl->send_resume_bitmap);
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

/* ── Sender: DataChannel open → send FILE_META then wait for resume ─ */

static void sender_on_dc_open(xDataChannel channel, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;

  /* When the DataChannel was queued (pending), impl->dc may be NULL.
     Update it now that the channel is actually open. */
  impl->dc = channel;

  set_state(impl, xTransferState_Transferring);

  /* Send FILE_META */
  xTransferFileMeta meta;
  memset(&meta, 0, sizeof(meta));
  size_t name_len = strlen(impl->send_filename);
  if (name_len > 255) name_len = 255;
  memcpy(meta.filename, impl->send_filename, name_len);
  meta.filename_len = (uint16_t)name_len;
  meta.file_size = impl->send_filesize;
  meta.chunk_size = impl->send_chunk_size;
  memcpy(meta.sha256, impl->send_sha256, XFER_SHA256_SIZE);

  uint8_t buf[512];
  size_t  len = 0;
  if (xTransferEncodeFileMeta(&meta, buf, sizeof(buf), &len) != xErrno_Ok) {
    report_error(impl, xErrno_Unknown, "Failed to encode FILE_META");
    return;
  }

  xDataChannelSendBinary(impl->dc, buf, len);

  /* Wait for FILE_RESUME from receiver before sending chunks.
     The receiver will inspect its local state and reply with a bitmap
     indicating which chunks it already has. */
  impl->send_waiting_resume = true;
}

/* ── Receiver: DataChannel message handler ─────────────── */

static void receiver_on_dc_message(xDataChannel channel,
                                   xDataChannelMsgType type,
                                   const uint8_t *data, size_t len,
                                   void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)channel;
  (void)type;

  if (len < 1) return;

  uint8_t msg_type = data[0];
  const uint8_t *payload = data + 1;
  size_t payload_len = len - 1;

  switch (msg_type) {
  case XFER_MSG_FILE_META: {
    xTransferFileMeta meta;
    if (xTransferDecodeFileMeta(payload, payload_len, &meta) != xErrno_Ok) {
      report_error(impl, xErrno_InvalidArg, "Invalid FILE_META");
      return;
    }

    /* Store metadata */
    memcpy(impl->recv_filename, meta.filename, meta.filename_len);
    impl->recv_filename[meta.filename_len] = '\0';
    impl->recv_filesize = meta.file_size;
    impl->recv_chunk_size = meta.chunk_size;
    impl->recv_total_chunks =
      (uint32_t)((meta.file_size + meta.chunk_size - 1) / meta.chunk_size);
    memcpy(impl->recv_sha256, meta.sha256, XFER_SHA256_SIZE);

    /* Notify callback */
    if (impl->conf.on_file_meta) {
      impl->conf.on_file_meta((xTransfer)impl, impl->recv_filename,
                              impl->recv_filesize, impl->conf.ctx);
    }

    /* Build file paths */
    snprintf(impl->recv_filepath, sizeof(impl->recv_filepath),
             "%s/%s.part", impl->recv_dest_dir, impl->recv_filename);
    snprintf(impl->recv_bitmap_path, sizeof(impl->recv_bitmap_path),
             "%s/%s.bitmap", impl->recv_dest_dir, impl->recv_filename);

    /* Try to load existing bitmap for resume */
    uint32_t saved_total = 0;
    xErrno bm_err = bitmap_load(&impl->recv_bitmap, &saved_total,
                                impl->recv_bitmap_path);
    if (bm_err == xErrno_Ok && saved_total == impl->recv_total_chunks) {
      /* Resume: reopen the .part file for random-access writing */
      impl->recv_fp = fopen(impl->recv_filepath, "r+b");
      if (!impl->recv_fp) {
        /* .part file missing but bitmap exists — start fresh */
        xBitmapFree(&impl->recv_bitmap);
        bm_err = xErrno_NotFound;
      } else {
        impl->recv_chunks_received = xBitmapCount(&impl->recv_bitmap);
        impl->recv_bytes_received =
          (uint64_t)impl->recv_chunks_received * impl->recv_chunk_size;
        if (impl->recv_bytes_received > impl->recv_filesize)
          impl->recv_bytes_received = impl->recv_filesize;
        XDEBUG("[xfer] receiver: resuming, %u/%u chunks already received",
               impl->recv_chunks_received, impl->recv_total_chunks);
      }
    } else {
      if (bm_err == xErrno_Ok) {
        /* total_chunks mismatch — discard old bitmap */
        xBitmapFree(&impl->recv_bitmap);
      }
      bm_err = xErrno_NotFound;
    }

    if (bm_err != xErrno_Ok) {
      /* Fresh transfer: create new bitmap and .part file */
      xErrno err = xBitmapInit(&impl->recv_bitmap, impl->recv_total_chunks);
      if (err != xErrno_Ok) {
        report_error(impl, err, "Failed to init recv bitmap");
        return;
      }
      impl->recv_fp = fopen(impl->recv_filepath, "wb");
      if (!impl->recv_fp) {
        report_error(impl, xErrno_SysError, "Failed to open output file");
        return;
      }
      /* Pre-allocate the sparse file by seeking to the end */
      if (impl->recv_filesize > 0) {
        fseek(impl->recv_fp, (long)(impl->recv_filesize - 1), SEEK_SET);
        fputc(0, impl->recv_fp);
        fflush(impl->recv_fp);
      }
      /* Reopen as "r+b" for random-access chunk writes */
      fclose(impl->recv_fp);
      impl->recv_fp = fopen(impl->recv_filepath, "r+b");
      if (!impl->recv_fp) {
        report_error(impl, xErrno_SysError, "Failed to reopen .part file");
        return;
      }
    }

    /* Send FILE_RESUME to sender with our bitmap */
    {
      uint32_t bm_nbytes = 0;
      const uint8_t *bm_data = xBitmapData(&impl->recv_bitmap, &bm_nbytes);

      xTransferFileResume resume;
      resume.total_chunks = impl->recv_total_chunks;
      resume.bitmap = bm_data;
      resume.bitmap_len = bm_nbytes;

      /* Allocate buffer: 1 + 4 + 4 + bitmap_len */
      size_t resume_buf_size = 9 + bm_nbytes;
      uint8_t *resume_buf = (uint8_t *)malloc(resume_buf_size);
      if (!resume_buf) {
        report_error(impl, xErrno_NoMemory,
                     "Failed to allocate FILE_RESUME buffer");
        return;
      }

      size_t resume_len = 0;
      if (xTransferEncodeFileResume(&resume, resume_buf, resume_buf_size,
                                    &resume_len) != xErrno_Ok) {
        free(resume_buf);
        report_error(impl, xErrno_Unknown, "Failed to encode FILE_RESUME");
        return;
      }

      xDataChannelSendBinary(impl->dc, resume_buf, resume_len);
      free(resume_buf);
    }

    /* Save initial bitmap to disk */
    bitmap_save(&impl->recv_bitmap, impl->recv_total_chunks,
                impl->recv_bitmap_path);

    set_state(impl, xTransferState_Transferring);
    break;
  }

  case XFER_MSG_FILE_CHUNK: {
    uint32_t       chunk_id;
    const uint8_t *chunk_data;
    uint32_t       chunk_data_len;

    if (xTransferDecodeChunkHeader(payload, payload_len, &chunk_id,
                                   &chunk_data, &chunk_data_len) != xErrno_Ok) {
      report_error(impl, xErrno_InvalidArg, "Invalid FILE_CHUNK");
      return;
    }

    /* Skip if already received (duplicate) */
    if (xBitmapTest(&impl->recv_bitmap, chunk_id)) {
      XDEBUG("[xfer] receiver: skipping duplicate chunk %u", chunk_id);
      break;
    }

    /* Seek to the correct offset and write */
    if (impl->recv_fp) {
      uint64_t offset = (uint64_t)chunk_id * impl->recv_chunk_size;
      if (fseek(impl->recv_fp, (long)offset, SEEK_SET) != 0) {
        report_error(impl, xErrno_SysError, "Failed to seek in output file");
        return;
      }
      size_t written = fwrite(chunk_data, 1, chunk_data_len, impl->recv_fp);
      if (written != chunk_data_len) {
        report_error(impl, xErrno_SysError, "Failed to write chunk");
        return;
      }
      fflush(impl->recv_fp);
    }

    /* Update bitmap and persist */
    xBitmapSet(&impl->recv_bitmap, chunk_id);
    impl->recv_chunks_received++;
    impl->recv_bytes_received += chunk_data_len;

    /* Persist bitmap periodically (every chunk for safety) */
    bitmap_save(&impl->recv_bitmap, impl->recv_total_chunks,
                impl->recv_bitmap_path);

    report_progress(impl, impl->recv_bytes_received, impl->recv_filesize);
    break;
  }

  case XFER_MSG_FILE_DONE: {
    xTransferFileDone done;
    if (xTransferDecodeFileDone(payload, payload_len, &done) != xErrno_Ok) {
      report_error(impl, xErrno_InvalidArg, "Invalid FILE_DONE");
      return;
    }

    /* Close file */
    if (impl->recv_fp) {
      fclose(impl->recv_fp);
      impl->recv_fp = NULL;
    }

    /* TODO: verify SHA-256 */

    /* Rename .part → final filename */
    char final_path[1024];
    snprintf(final_path, sizeof(final_path), "%s/%s",
             impl->recv_dest_dir, impl->recv_filename);
    rename(impl->recv_filepath, final_path);

    /* Remove .bitmap file */
    remove(impl->recv_bitmap_path);

    /* Free bitmap */
    xBitmapFree(&impl->recv_bitmap);

    set_state(impl, xTransferState_Done);
    break;
  }

  default:
    xLog(false, "xfer: unknown message type: 0x%02x", msg_type);
    break;
  }
}

/* ── PeerConnection callbacks ──────────────────────────── */

static void on_pc_state_change(xPeerConnection pc, xPeerConnectionState state,
                               void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)pc;

  switch (state) {
  case xPeerConnectionState_Connecting:
    set_state(impl, xTransferState_Connecting);
    break;
  case xPeerConnectionState_Connected:
    /* DataChannel open callback will move to Transferring */
    break;
  case xPeerConnectionState_Failed:
    report_error(impl, xErrno_Unknown, "PeerConnection failed");
    break;
  case xPeerConnectionState_Disconnected:
  case xPeerConnectionState_Closed:
    if (impl->state != xTransferState_Done) {
      report_error(impl, xErrno_Unknown, "PeerConnection closed unexpectedly");
    }
    break;
  default:
    break;
  }
}

static void on_pc_datachannel(xPeerConnection pc, xDataChannel channel,
                              void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)pc;

  /* Receiver side: remote DataChannel opened by sender */
  impl->dc = channel;
}

static void on_pc_ice_candidate(xPeerConnection pc, const char *candidate,
                                void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)pc;

  /* Forward candidate via signaling if connected */
  if (impl->signal && candidate) {
    xSignalClientSendCandidate(impl->signal, candidate);
  }

  if (impl->conf.on_ice_candidate) {
    impl->conf.on_ice_candidate((xTransfer)impl, candidate, impl->conf.ctx);
  }
}

/* ── Signaling callbacks (used when signal_server is configured) ──── */

static void on_signal_code(xSignalClient client, const char *code, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;

  strncpy(impl->code, code, XFER_MAX_CODE_LEN - 1);
  XDEBUG("[xfer] signaling: received code=%s", code);

  if (impl->conf.on_code) {
    impl->conf.on_code((xTransfer)impl, code, impl->conf.ctx);
  }
}

static void on_signal_peer_joined(xSignalClient client, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;

  XDEBUG("[xfer] signaling: peer joined, starting SDP exchange");

  /* Sender: create offer, set local, send via signaling, gather ICE */
  char *offer = xPeerConnectionCreateOffer(impl->pc);
  if (!offer) {
    report_error(impl, xErrno_Unknown, "Failed to create SDP offer");
    return;
  }

  xPeerConnectionSetLocalDescription(impl->pc, offer);
  xSignalClientSendOffer(impl->signal, offer);
  free(offer);

  xIceAgentGather(xPeerConnectionGetIceAgent(impl->pc));
}

static void on_signal_offer(xSignalClient client, const char *sdp, void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;

  XDEBUG("[xfer] signaling: received SDP offer");

  xPeerConnectionSetRemoteDescription(impl->pc, sdp);

  /* Receiver: create answer, set local, send via signaling, gather ICE */
  char *answer = xPeerConnectionCreateAnswer(impl->pc);
  if (!answer) {
    report_error(impl, xErrno_Unknown, "Failed to create SDP answer");
    return;
  }

  xPeerConnectionSetLocalDescription(impl->pc, answer);
  xSignalClientSendAnswer(impl->signal, answer);
  free(answer);

  xIceAgentGather(xPeerConnectionGetIceAgent(impl->pc));
}

static void on_signal_answer(xSignalClient client, const char *sdp,
                             void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;

  XDEBUG("[xfer] signaling: received SDP answer");
  xPeerConnectionSetRemoteDescription(impl->pc, sdp);
}

static void on_signal_candidate(xSignalClient client, const char *candidate,
                                void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;

  XDEBUG("[xfer] signaling: received remote ICE candidate");
  xIceAgentAddRemoteCandidate(xPeerConnectionGetIceAgent(impl->pc), candidate);
}

static void on_signal_error(xSignalClient client, xErrno err, const char *msg,
                            void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)client;
  (void)err;

  report_error(impl, xErrno_Unknown, msg);
}

/**
 * @brief Connect to the signaling server (if configured).
 *
 * @param impl  Transfer instance.
 * @param role  Signaling role (sender or receiver).
 * @param code  Code for joining (receiver only, NULL for sender).
 * @return      xErrno_Ok on success, or if no signal_server configured.
 */
static xErrno connect_signaling(xTransfer_ *impl, xSignalClientRole role,
                                const char *code) {
  if (!impl->conf.signal_server) return xErrno_Ok;

  xSignalClientConf sc_conf;
  memset(&sc_conf, 0, sizeof(sc_conf));
  sc_conf.url            = impl->conf.signal_server;
  sc_conf.role           = role;
  sc_conf.code           = code;
  sc_conf.on_code        = on_signal_code;
  sc_conf.on_peer_joined = on_signal_peer_joined;
  sc_conf.on_offer       = on_signal_offer;
  sc_conf.on_answer      = on_signal_answer;
  sc_conf.on_candidate   = on_signal_candidate;
  sc_conf.on_error       = on_signal_error;
  sc_conf.ctx            = impl;

  impl->signal = xSignalClientCreate(impl->loop, &sc_conf);
  if (!impl->signal) {
    return xErrno_Unknown;
  }

  return xErrno_Ok;
}

/* ───────────────────── Public API ───────────────────── */

xTransfer xTransferCreate(xEventLoop loop, const xTransferConf *conf) {
  if (!loop || !conf) return NULL;

  xTransfer_ *impl = (xTransfer_ *)calloc(1, sizeof(xTransfer_));
  if (!impl) return NULL;

  impl->loop = loop;
  impl->conf = *conf;
  impl->state = xTransferState_Idle;
  impl->send_chunk_size = XFER_DEFAULT_CHUNK_SIZE;

  return (xTransfer)impl;
}

void xTransferDestroy(xTransfer xfer) {
  if (!xfer) return;
  xTransfer_ *impl = (xTransfer_ *)xfer;

  if (impl->send_fp) {
    fclose(impl->send_fp);
    impl->send_fp = NULL;
  }
  if (impl->recv_fp) {
    fclose(impl->recv_fp);
    impl->recv_fp = NULL;
  }
  if (impl->signal) {
    xSignalClientDestroy(impl->signal);
    impl->signal = NULL;
  }
  if (impl->pc) {
    xPeerConnectionDestroy(impl->pc);
    impl->pc = NULL;
  }

  /* Free bitmaps */
  xBitmapFree(&impl->send_resume_bitmap);
  xBitmapFree(&impl->recv_bitmap);

  free(impl);
}

xErrno xTransferSendFile(xTransfer xfer, const char *filepath) {
  if (!xfer || !filepath) return xErrno_InvalidArg;
  xTransfer_ *impl = (xTransfer_ *)xfer;

  if (impl->state != xTransferState_Idle) return xErrno_InvalidState;

  impl->role = xTransferRole_Sender;

  /* Store filepath and extract filename */
  strncpy(impl->send_filepath, filepath, sizeof(impl->send_filepath) - 1);
  const char *name = basename_of(filepath);
  strncpy(impl->send_filename, name, XFER_MAX_FILENAME_LEN - 1);

  /* Open file and get size */
  impl->send_fp = fopen(filepath, "rb");
  if (!impl->send_fp) {
    report_error(impl, xErrno_SysError, "Failed to open file");
    return xErrno_SysError;
  }

  fseek(impl->send_fp, 0, SEEK_END);
  impl->send_filesize = (uint64_t)ftell(impl->send_fp);
  fseek(impl->send_fp, 0, SEEK_SET);

  impl->send_total_chunks =
    (uint32_t)((impl->send_filesize + impl->send_chunk_size - 1) /
               impl->send_chunk_size);
  impl->send_next_chunk = 0;

  /* TODO: compute SHA-256 of the file */

  /* Create PeerConnection */
  xPeerConnectionConf pc_conf;
  memset(&pc_conf, 0, sizeof(pc_conf));
  pc_conf.stun_server = impl->conf.stun_server;
  pc_conf.turn_server = impl->conf.turn_server;
  pc_conf.turn_username = impl->conf.turn_username;
  pc_conf.turn_password = impl->conf.turn_password;
  pc_conf.enable_ipv6 = impl->conf.enable_ipv6;
  pc_conf.on_state_change = on_pc_state_change;
  pc_conf.on_ice_candidate = on_pc_ice_candidate;
  pc_conf.on_datachannel = on_pc_datachannel;
  pc_conf.on_dc_open = sender_on_dc_open;
  pc_conf.ctx = impl;

  impl->pc = xPeerConnectionCreate(impl->loop, &pc_conf);
  if (!impl->pc) {
    report_error(impl, xErrno_Unknown, "Failed to create PeerConnection");
    return xErrno_Unknown;
  }

  /* Create DataChannel */
  xDataChannelConf dc_conf;
  memset(&dc_conf, 0, sizeof(dc_conf));
  strncpy(dc_conf.label, "xfer", XDC_MAX_LABEL_LEN - 1);
  dc_conf.ordered = true;
  dc_conf.on_open = sender_on_dc_open;
  dc_conf.on_buffered_amount_low = sender_on_buffered_amount_low;
  dc_conf.on_message = sender_on_dc_message;
  dc_conf.ctx = impl;

  /* DataChannel may be queued (returns NULL) until SCTP connects.
     The PeerConnection will create it later and fire on_dc_open. */
  impl->dc = xPeerConnectionCreateDataChannel(impl->pc, &dc_conf);

  set_state(impl, xTransferState_WaitingPeer);

  /* Connect to signaling server if configured */
  xErrno sig_err = connect_signaling(impl, xSignalClientRole_Sender, NULL);
  if (sig_err != xErrno_Ok) {
    report_error(impl, sig_err, "Failed to connect to signaling server");
    return sig_err;
  }

  return xErrno_Ok;
}

xErrno xTransferRecvFile(xTransfer xfer, const char *code,
                         const char *dest_dir) {
  if (!xfer || !code || !dest_dir) return xErrno_InvalidArg;
  xTransfer_ *impl = (xTransfer_ *)xfer;

  if (impl->state != xTransferState_Idle) return xErrno_InvalidState;

  impl->role = xTransferRole_Receiver;
  strncpy(impl->code, code, XFER_MAX_CODE_LEN - 1);
  strncpy(impl->recv_dest_dir, dest_dir, sizeof(impl->recv_dest_dir) - 1);

  /* Create PeerConnection */
  xPeerConnectionConf pc_conf;
  memset(&pc_conf, 0, sizeof(pc_conf));
  pc_conf.stun_server = impl->conf.stun_server;
  pc_conf.turn_server = impl->conf.turn_server;
  pc_conf.turn_username = impl->conf.turn_username;
  pc_conf.turn_password = impl->conf.turn_password;
  pc_conf.enable_ipv6 = impl->conf.enable_ipv6;
  pc_conf.on_state_change = on_pc_state_change;
  pc_conf.on_ice_candidate = on_pc_ice_candidate;
  pc_conf.on_datachannel = on_pc_datachannel;
  pc_conf.on_dc_message = receiver_on_dc_message;
  pc_conf.ctx = impl;

  impl->pc = xPeerConnectionCreate(impl->loop, &pc_conf);
  if (!impl->pc) {
    report_error(impl, xErrno_Unknown, "Failed to create PeerConnection");
    return xErrno_Unknown;
  }

  set_state(impl, xTransferState_WaitingPeer);

  /* Connect to signaling server if configured */
  xErrno sig_err = connect_signaling(impl, xSignalClientRole_Receiver, code);
  if (sig_err != xErrno_Ok) {
    report_error(impl, sig_err, "Failed to connect to signaling server");
    return sig_err;
  }

  return xErrno_Ok;
}

xTransferState xTransferGetState(xTransfer xfer) {
  if (!xfer) return xTransferState_Idle;
  return ((xTransfer_ *)xfer)->state;
}

xTransferRole xTransferGetRole(xTransfer xfer) {
  if (!xfer) return xTransferRole_Sender;
  return ((xTransfer_ *)xfer)->role;
}

void xTransferCancel(xTransfer xfer) {
  if (!xfer) return;
  xTransfer_ *impl = (xTransfer_ *)xfer;

  if (impl->state == xTransferState_Done ||
      impl->state == xTransferState_Failed) {
    return;
  }

  if (impl->dc) {
    xDataChannelClose(impl->dc);
    impl->dc = NULL;
  }

  set_state(impl, xTransferState_Failed);
}

/* ───────────────────── SDP / ICE API ───────────────────── */

char *xTransferCreateOffer(xTransfer xfer) {
  if (!xfer) return NULL;
  xTransfer_ *impl = (xTransfer_ *)xfer;
  if (!impl->pc) return NULL;
  return xPeerConnectionCreateOffer(impl->pc);
}

char *xTransferCreateAnswer(xTransfer xfer) {
  if (!xfer) return NULL;
  xTransfer_ *impl = (xTransfer_ *)xfer;
  if (!impl->pc) return NULL;
  return xPeerConnectionCreateAnswer(impl->pc);
}

xErrno xTransferSetLocalDescription(xTransfer xfer, const char *sdp) {
  if (!xfer || !sdp) return xErrno_InvalidArg;
  xTransfer_ *impl = (xTransfer_ *)xfer;
  if (!impl->pc) return xErrno_InvalidState;
  return xPeerConnectionSetLocalDescription(impl->pc, sdp);
}

xErrno xTransferSetRemoteDescription(xTransfer xfer, const char *sdp) {
  if (!xfer || !sdp) return xErrno_InvalidArg;
  xTransfer_ *impl = (xTransfer_ *)xfer;
  if (!impl->pc) return xErrno_InvalidState;
  return xPeerConnectionSetRemoteDescription(impl->pc, sdp);
}

xErrno xTransferGatherCandidates(xTransfer xfer) {
  if (!xfer) return xErrno_InvalidArg;
  xTransfer_ *impl = (xTransfer_ *)xfer;
  if (!impl->pc) return xErrno_InvalidState;
  return xIceAgentGather(xPeerConnectionGetIceAgent(impl->pc));
}
