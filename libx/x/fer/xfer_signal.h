/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_signal.h - Signaling server & client for P2P file transfer
 *
 * The signaling server acts as a rendezvous point for sender and
 * receiver peers. It relays SDP offers/answers and ICE candidates
 * over WebSocket connections.
 *
 * The signaling client is used internally by xfer.c to connect to
 * the signaling server and perform the SDP/ICE exchange.
 */

#ifndef XFER_XFER_SIGNAL_H
#define XFER_XFER_SIGNAL_H

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Signaling Server
 * ═══════════════════════════════════════════════════════════════════════ */

XDEF_HANDLE(xSignalServer);

/**
 * @brief Configuration for the signaling server.
 */
XDEF_STRUCT(xSignalServerConf) {
  const char *host; /**< Bind address (NULL = "0.0.0.0").       */
  uint16_t    port; /**< Port to listen on.                     */
};

/**
 * @brief Create and start a signaling server.
 *
 * The server listens for WebSocket connections on the configured
 * address and port. It handles session creation (sender) and
 * joining (receiver), and relays SDP/ICE messages between peers.
 *
 * @param loop  Event loop (must not be NULL).
 * @param conf  Server configuration (must not be NULL).
 * @return      Server handle, or NULL on failure.
 */
XCAPI(xSignalServer) xSignalServerCreate(xEventLoop loop, const xSignalServerConf *conf);

/**
 * @brief Destroy the signaling server and free all resources.
 *
 * @param server  Server handle, or NULL (no-op).
 */
XCAPI(void) xSignalServerDestroy(xSignalServer server);

/* ═══════════════════════════════════════════════════════════════════════
 *  Signaling Client
 * ═══════════════════════════════════════════════════════════════════════ */

XDEF_HANDLE(xSignalClient);

/**
 * @brief Signaling client role.
 */
XDEF_ENUM(xSignalClientRole){
  xSignalClientRole_Sender   = 0,
  xSignalClientRole_Receiver = 1,
};

/* ── Callbacks ─────────────────────────────────────────── */

/**
 * @brief Called when the client is connected to the signaling server.
 *
 * For senders, this is called after the "code" message is received.
 * For receivers, this is called after the "joined" message is received.
 */
typedef void (*xSignalOnConnected)(xSignalClient client, void *ctx);

/**
 * @brief Called when the sender receives a code from the server.
 */
typedef void (*xSignalOnCode)(xSignalClient client, const char *code, void *ctx);

/**
 * @brief Called when the peer has joined the session.
 */
typedef void (*xSignalOnPeerJoined)(xSignalClient client, void *ctx);

/**
 * @brief Called when an SDP offer is received from the peer.
 */
typedef void (*xSignalOnOffer)(xSignalClient client, const char *sdp, void *ctx);

/**
 * @brief Called when an SDP answer is received from the peer.
 */
typedef void (*xSignalOnAnswer)(xSignalClient client, const char *sdp, void *ctx);

/**
 * @brief Called when an ICE candidate is received from the peer.
 */
typedef void (*xSignalOnCandidate)(xSignalClient client, const char *candidate, void *ctx);

/**
 * @brief Called when the signaling connection encounters an error.
 */
typedef void (*xSignalOnError)(xSignalClient client, xErrno err, const char *msg, void *ctx);

/**
 * @brief Configuration for the signaling client.
 */
XDEF_STRUCT(xSignalClientConf) {
  /** WebSocket URL of the signaling server (e.g. "ws://host:port/ws"). */
  const char *url;

  /** Client role: sender or receiver. */
  xSignalClientRole role;

  /**
   * Code for joining a session (receiver only).
   * Ignored for senders.
   */
  const char *code;

  /** Callbacks. */
  xSignalOnConnected  on_connected;
  xSignalOnCode       on_code;        /**< Sender only.   */
  xSignalOnPeerJoined on_peer_joined; /**< Sender only.   */
  xSignalOnOffer      on_offer;       /**< Receiver only. */
  xSignalOnAnswer     on_answer;      /**< Sender only.   */
  xSignalOnCandidate  on_candidate;   /**< Both.          */
  xSignalOnError      on_error;       /**< Both.          */
  void               *ctx;            /**< User context.  */
};

/**
 * @brief Create a signaling client and connect to the server.
 *
 * Initiates an asynchronous WebSocket connection to the signaling
 * server. For senders, a "create" message is sent upon connection;
 * for receivers, a "join" message with the code is sent.
 *
 * @param loop  Event loop (must not be NULL).
 * @param conf  Client configuration (must not be NULL).
 * @return      Client handle, or NULL on failure.
 */
XCAPI(xSignalClient) xSignalClientCreate(xEventLoop loop, const xSignalClientConf *conf);

/**
 * @brief Destroy the signaling client and close the connection.
 *
 * @param client  Client handle, or NULL (no-op).
 */
XCAPI(void) xSignalClientDestroy(xSignalClient client);

/**
 * @brief Send an SDP offer through the signaling server.
 *
 * @param client  Client handle (must not be NULL).
 * @param sdp     SDP string (must not be NULL).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSignalClientSendOffer(xSignalClient client, const char *sdp);

/**
 * @brief Send an SDP answer through the signaling server.
 *
 * @param client  Client handle (must not be NULL).
 * @param sdp     SDP string (must not be NULL).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xSignalClientSendAnswer(xSignalClient client, const char *sdp);

/**
 * @brief Send an ICE candidate through the signaling server.
 *
 * @param client     Client handle (must not be NULL).
 * @param candidate  ICE candidate string (must not be NULL).
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xSignalClientSendCandidate(xSignalClient client, const char *candidate);

#endif /* XFER_XFER_SIGNAL_H */
