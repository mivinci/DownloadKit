/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer.c - P2P file transfer core implementation
 *
 * Orchestrates PeerConnection setup, DataChannel creation, signaling,
 * and the public API.  Sender / receiver logic lives in xfer_sender.c
 * and xfer_receiver.c respectively.
 */

#include "xfer_private.h"
#include "xfer_signal.h"

#include <xbase/log.h>
#include <xcrypto/sha1.h>
#include <xp2p/peer_connection.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared helpers ────────────────────────────────────── */

void xfer_set_state(xTransfer_ *impl, xTransferState state) {
  impl->state = state;
  if (impl->conf.on_state_change) {
    impl->conf.on_state_change((xTransfer)impl, state, impl->conf.ctx);
  }
}

void xfer_report_error(xTransfer_ *impl, xErrno err, const char *msg) {
  if (impl->conf.on_error) {
    impl->conf.on_error((xTransfer)impl, err, msg, impl->conf.ctx);
  }
  xfer_set_state(impl, xTransferState_Failed);
}

void xfer_report_progress(xTransfer_ *impl, uint64_t transferred,
                            uint64_t total) {
  if (impl->conf.on_progress) {
    impl->conf.on_progress((xTransfer)impl, transferred, total, impl->conf.ctx);
  }
}

/**
 * @brief Compute SHA-1 of an entire file by streaming through it.
 *
 * Reads the file in 64 KiB blocks so arbitrarily large files can be
 * hashed without loading them entirely into memory.
 */
xErrno xfer_compute_file_sha1(const xTransferVfs *vfs, const char *path,
                                uint8_t *digest) {
  void *handle = vfs->open(vfs->ctx, path, "rb");
  if (!handle) return xErrno_SysError;

  xSha1Ctx ctx;
  xSha1Init(&ctx);

  uint8_t buf[65536];
  uint64_t offset = 0;
  size_t n = 0;
  xErrno err;
  for (;;) {
    err = vfs->pread(vfs->ctx, handle, buf, sizeof(buf), offset, &n);
    if (err != xErrno_Ok) { vfs->close(vfs->ctx, handle); return err; }
    if (n == 0) break;
    xSha1Update(&ctx, buf, n);
    offset += n;
  }

  vfs->close(vfs->ctx, handle);
  xSha1Final(&ctx, digest);
  return xErrno_Ok;
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
xErrno xfer_bitmap_save(const xBitmap *bm, uint32_t total_chunks,
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
xErrno xfer_bitmap_load(xBitmap *bm, uint32_t *total_chunks,
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

/* ── PeerConnection callbacks ──────────────────────────── */

static void on_pc_state_change(xPeerConnection pc, xPeerConnectionState state,
                               void *ctx) {
  xTransfer_ *impl = (xTransfer_ *)ctx;
  (void)pc;

  switch (state) {
  case xPeerConnectionState_Connecting:
    xfer_set_state(impl, xTransferState_Connecting);
    break;
  case xPeerConnectionState_Connected:
    /* DataChannel open callback will move to Transferring */
    break;
  case xPeerConnectionState_Failed:
    xfer_report_error(impl, xErrno_Unknown, "PeerConnection failed");
    break;
  case xPeerConnectionState_Disconnected:
  case xPeerConnectionState_Closed:
    if (impl->state != xTransferState_Done &&
        impl->state != xTransferState_Failed) {
      xfer_report_error(impl, xErrno_Unknown,
                        "PeerConnection closed unexpectedly");
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
    xfer_report_error(impl, xErrno_Unknown, "Failed to create SDP offer");
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
    xfer_report_error(impl, xErrno_Unknown, "Failed to create SDP answer");
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

  xfer_report_error(impl, xErrno_Unknown, msg);
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
  impl->vfs = conf->vfs ? conf->vfs : xTransferPosixVfs();

  return (xTransfer)impl;
}

void xTransferDestroy(xTransfer xfer) {
  if (!xfer) return;
  xTransfer_ *impl = (xTransfer_ *)xfer;

  if (impl->send_handle) {
    impl->vfs->close(impl->vfs->ctx, impl->send_handle);
    impl->send_handle = NULL;
  }
  if (impl->recv_handle) {
    impl->vfs->close(impl->vfs->ctx, impl->recv_handle);
    impl->recv_handle = NULL;
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
  impl->send_handle = impl->vfs->open(impl->vfs->ctx, filepath, "rb");
  if (!impl->send_handle) {
    xfer_report_error(impl, xErrno_SysError, "Failed to open file");
    return xErrno_SysError;
  }

  uint64_t fsize = 0;
  if (impl->vfs->size(impl->vfs->ctx, impl->send_handle, &fsize) != xErrno_Ok) {
    impl->vfs->close(impl->vfs->ctx, impl->send_handle);
    impl->send_handle = NULL;
    xfer_report_error(impl, xErrno_SysError, "Failed to get file size");
    return xErrno_SysError;
  }
  impl->send_filesize = fsize;

  impl->send_total_chunks =
    (uint32_t)((impl->send_filesize + impl->send_chunk_size - 1) /
               impl->send_chunk_size);
  impl->send_next_chunk = 0;

  /* SHA-1 will be computed incrementally during transfer.
   * The send_sha1 field in FILE_META is zeroed (computed later). */
  memset(impl->send_sha1, 0, XFER_SHA1_SIZE);

  /* Create PeerConnection */
  xPeerConnectionConf pc_conf;
  memset(&pc_conf, 0, sizeof(pc_conf));
  pc_conf.stun_server = impl->conf.stun_server;
  pc_conf.turn_server = impl->conf.turn_server;
  pc_conf.turn_username = impl->conf.turn_username;
  pc_conf.turn_password = impl->conf.turn_password;
  pc_conf.enable_ipv6 = impl->conf.enable_ipv6;
  pc_conf.birthday_k = impl->conf.birthday_k;
  pc_conf.birthday_n = impl->conf.birthday_n;
  pc_conf.on_state_change = on_pc_state_change;
  pc_conf.on_ice_candidate = on_pc_ice_candidate;
  pc_conf.on_datachannel = on_pc_datachannel;
  pc_conf.on_dc_open = sender_on_dc_open;
  pc_conf.ctx = impl;

  impl->pc = xPeerConnectionCreate(impl->loop, &pc_conf);
  if (!impl->pc) {
    xfer_report_error(impl, xErrno_Unknown,
                      "Failed to create PeerConnection");
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

  xfer_set_state(impl, xTransferState_WaitingPeer);

  /* Connect to signaling server if configured */
  xErrno sig_err = connect_signaling(impl, xSignalClientRole_Sender, NULL);
  if (sig_err != xErrno_Ok) {
    xfer_report_error(impl, sig_err,
                      "Failed to connect to signaling server");
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
  strncpy(impl->recv_dest_dir, dest_dir, sizeof(impl->recv_dest_dir) - 1);

  strncpy(impl->code, code, XFER_MAX_CODE_LEN - 1);
  const char *session_code = impl->code;

  /* Create PeerConnection */
  xPeerConnectionConf pc_conf;
  memset(&pc_conf, 0, sizeof(pc_conf));
  pc_conf.stun_server = impl->conf.stun_server;
  pc_conf.turn_server = impl->conf.turn_server;
  pc_conf.turn_username = impl->conf.turn_username;
  pc_conf.turn_password = impl->conf.turn_password;
  pc_conf.enable_ipv6 = impl->conf.enable_ipv6;
  pc_conf.birthday_k = impl->conf.birthday_k;
  pc_conf.birthday_n = impl->conf.birthday_n;
  pc_conf.on_state_change = on_pc_state_change;
  pc_conf.on_ice_candidate = on_pc_ice_candidate;
  pc_conf.on_datachannel = on_pc_datachannel;
  pc_conf.on_dc_message = receiver_on_dc_message;
  pc_conf.ctx = impl;

  impl->pc = xPeerConnectionCreate(impl->loop, &pc_conf);
  if (!impl->pc) {
    xfer_report_error(impl, xErrno_Unknown,
                      "Failed to create PeerConnection");
    return xErrno_Unknown;
  }

  xfer_set_state(impl, xTransferState_WaitingPeer);

  /* Connect to signaling server if configured */
  xErrno sig_err = connect_signaling(impl, xSignalClientRole_Receiver,
                                     session_code);
  if (sig_err != xErrno_Ok) {
    xfer_report_error(impl, sig_err,
                      "Failed to connect to signaling server");
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

  xfer_set_state(impl, xTransferState_Failed);
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
