/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer.h - P2P file transfer library
 *
 * Provides a high-level API for peer-to-peer file transfer built on
 * top of WebRTC DataChannels (xp2p). The library handles:
 *   - File chunking and reassembly
 *   - P2P connection via PeerConnection (ICE + DTLS + SCTP + DataChannel)
 *   - Progress reporting
 *
 * Signaling is decoupled: the sender registers with a signaling server
 * and receives a short code; the receiver uses that code to find the
 * sender and establish a P2P connection.
 */

#ifndef XFER_XFER_H
#define XFER_XFER_H

#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

/** Default chunk size for file transfer (64 KB). */
#define XFER_DEFAULT_CHUNK_SIZE (64 * 1024)

/** Maximum file name length. */
#define XFER_MAX_FILENAME_LEN 256

/** Maximum code length. */
#define XFER_MAX_CODE_LEN 128

/* ───────────────────── Opaque Handle ───────────────────── */

XDEF_HANDLE(xTransfer);

/* ───────────────────── Transfer State ───────────────────── */

XDEF_ENUM(xTransferState){
  xTransferState_Idle         = 0, /**< Created, not started.           */
  xTransferState_WaitingPeer  = 1, /**< Signaling, waiting for peer.    */
  xTransferState_Connecting   = 2, /**< P2P connection in progress.     */
  xTransferState_Transferring = 3, /**< Data flowing.                   */
  xTransferState_Done         = 4, /**< Transfer complete.              */
  xTransferState_Failed       = 5, /**< Unrecoverable error.            */
};

/* ───────────────────── Transfer Role ───────────────────── */

XDEF_ENUM(xTransferRole){
  xTransferRole_Sender   = 0, /**< Sending a file.   */
  xTransferRole_Receiver = 1, /**< Receiving a file.  */
};

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when the transfer state changes.
 */
typedef void (*xTransferOnStateChange)(xTransfer xfer, xTransferState state,
                                       void *ctx);

/**
 * @brief Called periodically to report transfer progress.
 */
typedef void (*xTransferOnProgress)(xTransfer xfer,
                                    uint64_t bytes_transferred,
                                    uint64_t bytes_total, void *ctx);

/**
 * @brief Called when the sender receives a code from the signaling
 *        server. The receiver uses this code to connect.
 */
typedef void (*xTransferOnCode)(xTransfer xfer, const char *code, void *ctx);

/**
 * @brief Called when the receiver learns the file metadata from the
 *        sender (file name, size, etc.) before data transfer begins.
 */
typedef void (*xTransferOnFileMeta)(xTransfer xfer, const char *filename,
                                    uint64_t filesize, void *ctx);

/**
 * @brief Called when the transfer encounters an error.
 */
typedef void (*xTransferOnError)(xTransfer xfer, xErrno err, const char *msg,
                                 void *ctx);

/**
 * @brief Called when a new local ICE candidate is gathered.
 *
 * When @p candidate is NULL, gathering is complete.
 */
typedef void (*xTransferOnIceCandidate)(xTransfer xfer, const char *candidate,
                                        void *ctx);

/* ───────────────────── Virtual File System ───────────────────── */

#include "xfer_vfs.h"

/* ───────────────────── Configuration ───────────────────── */

XDEF_STRUCT(xTransferConf) {
  /* P2P configuration. */
  const char *stun_server;   /**< STUN server(s) "host:port" or comma-separated
                                  list for port prediction, or NULL.    */
  const char *turn_server;   /**< TURN server "host:port" or NULL.       */
  const char *turn_username; /**< TURN credential username.              */
  const char *turn_password; /**< TURN credential password.              */
  bool        enable_ipv6;   /**< Enable IPv6 candidates (default: false). */

  /** Signaling server URL (e.g. "http://signal.example.com"). */
  const char *signal_server;

  /** Optional VFS for custom storage.  NULL = default POSIX file I/O. */
  const xTransferVfs *vfs;

  /** Callbacks. */
  xTransferOnStateChange on_state_change;
  xTransferOnProgress    on_progress;
  xTransferOnCode        on_code;      /**< Sender only.                 */
  xTransferOnFileMeta    on_file_meta; /**< Receiver only.               */
  xTransferOnError       on_error;
  xTransferOnIceCandidate on_ice_candidate; /**< ICE candidate callback. */
  void                  *ctx; /**< Forwarded to all callbacks.            */
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a new transfer session.
 *
 * @param loop  Event loop (required).
 * @param conf  Configuration (required).
 * @return      Transfer handle, or NULL on failure.
 */
XCAPI(xTransfer) xTransferCreate(xEventLoop loop,
                                 const xTransferConf *conf);

/**
 * @brief Destroy a transfer session and free all resources.
 *
 * @param xfer  Transfer handle, or NULL (no-op).
 */
XCAPI(void) xTransferDestroy(xTransfer xfer);

/* ───────────────────── Operations ───────────────────── */

/**
 * @brief Start sending a file.
 *
 * Registers with the signaling server and waits for a receiver to
 * connect. The on_code callback fires with the code that the receiver
 * needs to use.
 *
 * @param xfer      Transfer handle.
 * @param filepath  Path to the file to send.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferSendFile(xTransfer xfer, const char *filepath);

/**
 * @brief Start receiving a file.
 *
 * Uses the code to find the sender via the signaling server, then
 * establishes a P2P connection and receives the file.
 *
 * @param xfer      Transfer handle.
 * @param code      Code obtained from the sender.
 * @param dest_dir  Directory to save the received file (uses the
 *                  original filename from the sender).
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferRecvFile(xTransfer xfer, const char *code,
                                const char *dest_dir);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Get the current transfer state.
 */
XCAPI(xTransferState) xTransferGetState(xTransfer xfer);

/**
 * @brief Get the transfer role (sender or receiver).
 */
XCAPI(xTransferRole) xTransferGetRole(xTransfer xfer);

/**
 * @brief Cancel an in-progress transfer.
 *
 * Moves the state to Failed and tears down the P2P connection.
 */
XCAPI(void) xTransferCancel(xTransfer xfer);

/* ───────────────────── SDP Negotiation ───────────────────── */

/**
 * @brief Create an SDP offer for the underlying PeerConnection.
 *
 * Only valid for the sender role (after xTransferSendFile).
 * Caller must free the returned string with free().
 *
 * @param xfer  Transfer handle.
 * @return      Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xTransferCreateOffer(xTransfer xfer);

/**
 * @brief Create an SDP answer for the underlying PeerConnection.
 *
 * Only valid for the receiver role (after xTransferRecvFile).
 * Caller must free the returned string with free().
 *
 * @param xfer  Transfer handle.
 * @return      Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xTransferCreateAnswer(xTransfer xfer);

/**
 * @brief Set the local SDP description.
 *
 * @param xfer  Transfer handle.
 * @param sdp   Local SDP string.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferSetLocalDescription(xTransfer xfer, const char *sdp);

/**
 * @brief Set the remote SDP description.
 *
 * @param xfer  Transfer handle.
 * @param sdp   Remote SDP string.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferSetRemoteDescription(xTransfer xfer, const char *sdp);

/**
 * @brief Start ICE candidate gathering.
 *
 * Candidates are reported via the on_ice_candidate callback.
 *
 * @param xfer  Transfer handle.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xTransferGatherCandidates(xTransfer xfer);

#endif /* XFER_XFER_H */
