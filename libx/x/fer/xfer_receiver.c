/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_receiver.c - Receiver-side logic for P2P file transfer
 *
 * Handles FILE_META processing, chunk writing, bitmap tracking,
 * SHA-1 verification, and FILE_ACK / FILE_RESUME messaging.
 */

#include "xfer_private.h"

#include <x/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DataChannel: message handler ──────────────────────── */

void receiver_on_dc_message(xDataChannel channel, xDataChannelMsgType type,
                            const uint8_t *data, size_t len, void *ctx) {
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
      xfer_report_error(impl, xErrno_InvalidArg, "Invalid FILE_META");
      return;
    }

    /* Store metadata */
    memcpy(impl->recv_filename, meta.filename, meta.filename_len);
    impl->recv_filename[meta.filename_len] = '\0';
    impl->recv_filesize = meta.file_size;
    impl->recv_chunk_size = meta.chunk_size;
    impl->recv_total_chunks =
      (uint32_t)((meta.file_size + meta.chunk_size - 1) / meta.chunk_size);
    memcpy(impl->recv_sha1, meta.sha1, XFER_SHA1_SIZE);

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
    xErrno bm_err = xfer_bitmap_load(&impl->recv_bitmap, &saved_total,
                                     impl->recv_bitmap_path);
    if (bm_err == xErrno_Ok && saved_total == impl->recv_total_chunks) {
      /* Resume: reopen the .part file for random-access writing */
      impl->recv_handle = impl->vfs->open(impl->vfs->ctx,
                                          impl->recv_filepath, "r+b");
      if (!impl->recv_handle) {
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
        xfer_report_error(impl, err, "Failed to init recv bitmap");
        return;
      }
      impl->recv_handle = impl->vfs->open(impl->vfs->ctx,
                                          impl->recv_filepath, "wb");
      if (!impl->recv_handle) {
        xfer_report_error(impl, xErrno_SysError,
                          "Failed to open output file");
        return;
      }
      /* Pre-allocate the sparse file */
      if (impl->recv_filesize > 0 && impl->vfs->truncate) {
        impl->vfs->truncate(impl->vfs->ctx, impl->recv_handle,
                            impl->recv_filesize);
      }
      /* Reopen as "r+b" for random-access chunk writes */
      impl->vfs->close(impl->vfs->ctx, impl->recv_handle);
      impl->recv_handle = impl->vfs->open(impl->vfs->ctx,
                                          impl->recv_filepath, "r+b");
      if (!impl->recv_handle) {
        xfer_report_error(impl, xErrno_SysError,
                          "Failed to reopen .part file");
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
        xfer_report_error(impl, xErrno_NoMemory,
                          "Failed to allocate FILE_RESUME buffer");
        return;
      }

      size_t resume_len = 0;
      if (xTransferEncodeFileResume(&resume, resume_buf, resume_buf_size,
                                    &resume_len) != xErrno_Ok) {
        free(resume_buf);
        xfer_report_error(impl, xErrno_Unknown,
                          "Failed to encode FILE_RESUME");
        return;
      }

      xDataChannelSendBinary(impl->dc, resume_buf, resume_len);
      free(resume_buf);
    }

    /* Save initial bitmap to disk */
    xfer_bitmap_save(&impl->recv_bitmap, impl->recv_total_chunks,
                     impl->recv_bitmap_path);

    xfer_set_state(impl, xTransferState_Transferring);
    break;
  }

  case XFER_MSG_FILE_CHUNK: {
    uint32_t       chunk_id;
    const uint8_t *chunk_data;
    uint32_t       chunk_data_len;

    if (xTransferDecodeChunkHeader(payload, payload_len, &chunk_id,
                                   &chunk_data, &chunk_data_len) != xErrno_Ok) {
      xfer_report_error(impl, xErrno_InvalidArg, "Invalid FILE_CHUNK");
      return;
    }

    /* Skip if already received (duplicate) */
    if (xBitmapTest(&impl->recv_bitmap, chunk_id)) {
      XDEBUG("[xfer] receiver: skipping duplicate chunk %u", chunk_id);
      break;
    }

    /* Seek to the correct offset and write */
    if (impl->recv_handle) {
      uint64_t offset = (uint64_t)chunk_id * impl->recv_chunk_size;
      size_t written = 0;
      xErrno werr = impl->vfs->pwrite(impl->vfs->ctx, impl->recv_handle,
                                      chunk_data, chunk_data_len,
                                      offset, &written);
      if (werr != xErrno_Ok || written != chunk_data_len) {
        xfer_report_error(impl, xErrno_SysError, "Failed to write chunk");
        return;
      }
    }

    /* Update bitmap and persist */
    xBitmapSet(&impl->recv_bitmap, chunk_id);
    impl->recv_chunks_received++;
    impl->recv_bytes_received += chunk_data_len;

    /* Persist bitmap periodically to reduce disk I/O overhead.
     * On resume, at most XFER_BITMAP_PERSIST_INTERVAL chunks may be
     * re-transferred, which is an acceptable trade-off. */
    if (impl->recv_chunks_received % XFER_BITMAP_PERSIST_INTERVAL == 0) {
      xfer_bitmap_save(&impl->recv_bitmap, impl->recv_total_chunks,
                       impl->recv_bitmap_path);
    }

    xfer_report_progress(impl, impl->recv_bytes_received,
                         impl->recv_filesize);
    break;
  }

  case XFER_MSG_FILE_DONE: {
    xTransferFileDone done;
    if (xTransferDecodeFileDone(payload, payload_len, &done) != xErrno_Ok) {
      xfer_report_error(impl, xErrno_InvalidArg, "Invalid FILE_DONE");
      return;
    }

    /* Flush remaining data and persist final bitmap before closing */
    if (impl->recv_handle) {
      impl->vfs->flush(impl->vfs->ctx, impl->recv_handle);
      impl->vfs->close(impl->vfs->ctx, impl->recv_handle);
      impl->recv_handle = NULL;
    }
    xfer_bitmap_save(&impl->recv_bitmap, impl->recv_total_chunks,
                     impl->recv_bitmap_path);

    /* Verify SHA-1: compute hash over the received .part file and compare
       with the sender's hash.  This is done from the file rather than
       incrementally so that resume transfers are handled correctly. */
    {
      xTransferFileAck ack;
      memset(&ack, 0, sizeof(ack));

      uint8_t computed[XFER_SHA1_SIZE];
      if (xfer_compute_file_sha1(impl->vfs, impl->recv_filepath,
                                 computed) != xErrno_Ok) {
        ack.status = 1;
        uint8_t ack_buf[4];
        size_t ack_len = 0;
        xTransferEncodeFileAck(&ack, ack_buf, sizeof(ack_buf), &ack_len);
        xDataChannelSendBinary(impl->dc, ack_buf, ack_len);
        xfer_report_error(impl, xErrno_SysError,
                          "Failed to compute SHA-1 of received file");
        return;
      }

      if (memcmp(computed, done.sha1, XFER_SHA1_SIZE) != 0) {
        ack.status = 1;
        uint8_t ack_buf[4];
        size_t ack_len = 0;
        xTransferEncodeFileAck(&ack, ack_buf, sizeof(ack_buf), &ack_len);
        xDataChannelSendBinary(impl->dc, ack_buf, ack_len);
        xfer_report_error(impl, xErrno_Unknown, "SHA-1 verification failed");
        return;
      }
      XDEBUG("[xfer] receiver: SHA-1 verification passed");

      /* Send success ACK to sender */
      ack.status = 0;
      uint8_t ack_buf[4];
      size_t ack_len = 0;
      xTransferEncodeFileAck(&ack, ack_buf, sizeof(ack_buf), &ack_len);
      xDataChannelSendBinary(impl->dc, ack_buf, ack_len);
    }

    /* Rename .part → final filename */
    char final_path[1024];
    snprintf(final_path, sizeof(final_path), "%s/%s",
             impl->recv_dest_dir, impl->recv_filename);
    if (impl->vfs->rename) {
      impl->vfs->rename(impl->vfs->ctx, impl->recv_filepath, final_path);
    }

    /* Remove .bitmap file */
    if (impl->vfs->remove) {
      impl->vfs->remove(impl->vfs->ctx, impl->recv_bitmap_path);
    }

    /* Free bitmap */
    xBitmapFree(&impl->recv_bitmap);

    xfer_set_state(impl, xTransferState_Done);
    break;
  }

  default:
    xLog(false, "xfer: unknown message type: 0x%02x", msg_type);
    break;
  }
}
