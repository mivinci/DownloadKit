/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * peer_connection.h - WebRTC PeerConnection API
 *
 * Orchestrates the full WebRTC protocol stack:
 *   ICE (connectivity) → DTLS (encryption) → SCTP (transport) → DataChannel
 *
 * Mirrors the browser RTCPeerConnection API:
 *   - createOffer / createAnswer
 *   - setLocalDescription / setRemoteDescription
 *   - addIceCandidate
 *   - createDataChannel / ondatachannel
 *   - onicecandidate / oniceconnectionstatechange
 */

#ifndef XP2P_PEER_CONNECTION_H
#define XP2P_PEER_CONNECTION_H

#include "datachannel.h"
#include "dtls_transport.h"
#include "ice_agent.h"
#include "sctp_transport.h"

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Opaque Handle ───────────────────── */

XDEF_HANDLE(xPeerConnection);

/* ───────────────────── Connection States ───────────────────── */

XDEF_ENUM(xPeerConnectionState){
  xPeerConnectionState_New          = 0, /**< Initial state.                */
  xPeerConnectionState_Connecting   = 1, /**< ICE/DTLS/SCTP in progress.   */
  xPeerConnectionState_Connected    = 2, /**< DataChannel ready.            */
  xPeerConnectionState_Disconnected = 3, /**< Connectivity lost.            */
  xPeerConnectionState_Failed       = 4, /**< Unrecoverable failure.        */
  xPeerConnectionState_Closed       = 5, /**< Explicitly closed.            */
};

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when the overall connection state changes.
 */
typedef void (*xPeerConnectionOnStateChange)(xPeerConnection pc, xPeerConnectionState state,
                                             void *arg);

/**
 * @brief Called when a new local ICE candidate is gathered.
 *
 * When @p candidate_sdp is NULL, gathering is complete.
 */
typedef void (*xPeerConnectionOnIceCandidate)(xPeerConnection pc, const char *candidate_sdp,
                                              void *arg);

/**
 * @brief Called when the remote peer opens a DataChannel.
 */
typedef void (*xPeerConnectionOnDataChannel)(xPeerConnection pc, xDataChannel channel, void *arg);

/* ───────────────────── Configuration ───────────────────── */

XDEF_STRUCT(xPeerConnectionConf) {
  /** ICE configuration. */
  const char *stun_server;   /**< STUN server(s) "host:port" or comma-separated
                                  list for port prediction, or NULL.  */
  const char *turn_server;   /**< TURN server "host:port" or NULL.       */
  const char *turn_username; /**< TURN credential username.              */
  const char *turn_password; /**< TURN credential password.              */
  bool        enable_ipv6;   /**< Enable IPv6 candidates (default: false). */

  /** SCTP port (0 = default 5000). */
  uint16_t sctp_port;

  /** Callbacks. */
  xPeerConnectionOnStateChange  on_state_change;
  xPeerConnectionOnIceCandidate on_ice_candidate;
  xPeerConnectionOnDataChannel  on_datachannel;

  /** Default callbacks for remotely-opened DataChannels. */
  xDataChannelOnOpen    on_dc_open;
  xDataChannelOnMessage on_dc_message;
  xDataChannelOnClose   on_dc_close;
  void                 *ctx; /**< Forwarded to all callbacks. */
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a new PeerConnection.
 *
 * Internally creates an ICE agent and a DTLS transport (with a
 * self-signed certificate). Does NOT start gathering or handshake.
 *
 * @param loop  Event loop (required).
 * @param conf  Configuration (required).
 * @return      PeerConnection handle, or NULL on failure.
 */
XCAPI(xPeerConnection) xPeerConnectionCreate(xEventLoop loop, const xPeerConnectionConf *conf);

/**
 * @brief Destroy a PeerConnection and all owned resources.
 *
 * Tears down DataChannel manager, SCTP, DTLS, and ICE in order.
 *
 * @param pc  PeerConnection handle, or NULL (no-op).
 */
XCAPI(void) xPeerConnectionDestroy(xPeerConnection pc);

/* ───────────────────── SDP Negotiation ───────────────────── */

/**
 * @brief Create a WebRTC SDP offer.
 *
 * Starts ICE gathering if not already started. The returned SDP
 * includes ICE credentials, DTLS fingerprint, and SCTP parameters.
 *
 * Caller must free the returned string with free().
 *
 * @param pc  PeerConnection handle.
 * @return    Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xPeerConnectionCreateOffer(xPeerConnection pc);

/**
 * @brief Create a WebRTC SDP answer.
 *
 * Should be called after setRemoteDescription with the offer.
 * Caller must free the returned string with free().
 *
 * @param pc  PeerConnection handle.
 * @return    Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xPeerConnectionCreateAnswer(xPeerConnection pc);

/**
 * @brief Set the local SDP description.
 *
 * Starts ICE gathering if not already started.
 *
 * @param pc   PeerConnection handle.
 * @param sdp  Local SDP string (offer or answer).
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xPeerConnectionSetLocalDescription(xPeerConnection pc, const char *sdp);

/**
 * @brief Set the remote SDP description.
 *
 * Parses ICE credentials, DTLS fingerprint, setup role, and SCTP
 * port from the remote SDP. Adds remote ICE candidates.
 *
 * @param pc   PeerConnection handle.
 * @param sdp  Remote SDP string.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xPeerConnectionSetRemoteDescription(xPeerConnection pc, const char *sdp);

/**
 * @brief Add a remote ICE candidate (Trickle ICE).
 *
 * @param pc             PeerConnection handle.
 * @param candidate_sdp  SDP candidate line (e.g. "candidate:...").
 * @return               xErrno_Ok on success.
 */
XCAPI(xErrno) xPeerConnectionAddIceCandidate(xPeerConnection pc, const char *candidate_sdp);

/* ───────────────────── DataChannel ───────────────────── */

/**
 * @brief Create a new DataChannel.
 *
 * The channel will be opened once the SCTP association is established.
 *
 * @param pc    PeerConnection handle.
 * @param conf  DataChannel configuration.
 * @return      DataChannel handle, or NULL on failure.
 */
XCAPI(xDataChannel) xPeerConnectionCreateDataChannel(xPeerConnection         pc,
                                                     const xDataChannelConf *conf);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Get the current connection state.
 */
XCAPI(xPeerConnectionState) xPeerConnectionGetState(xPeerConnection pc);

/**
 * @brief Get the underlying ICE agent.
 */
XCAPI(xIceAgent) xPeerConnectionGetIceAgent(xPeerConnection pc);

/**
 * @brief Get the DTLS transport.
 */
XCAPI(xDtlsTransport) xPeerConnectionGetDtlsTransport(xPeerConnection pc);

/**
 * @brief Get the SCTP transport.
 */
XCAPI(xSctpTransport) xPeerConnectionGetSctpTransport(xPeerConnection pc);

/**
 * @brief Get the DataChannel manager.
 */
XCAPI(xDataChannelMgr) xPeerConnectionGetDataChannelMgr(xPeerConnection pc);

#endif /* XP2P_PEER_CONNECTION_H */
