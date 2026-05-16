/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_agent.h - xIce public API
 *
 * Provides ICE-based P2P connectivity. This module handles STUN/TURN
 * NAT traversal, candidate gathering, connectivity checks, and
 * nomination to establish a UDP transport between two peers.
 */

#ifndef XP2P_ICE_AGENT_H
#define XP2P_ICE_AGENT_H

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

#include "ice_candidate.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations — full definitions are internal. */
XDEF_HANDLE(xIceAgent);

/* ───────────────────── Agent States ───────────────────── */

XDEF_ENUM(xIceState){
  xIceState_New       = 0, /**< Initial state, no activity yet.              */
  xIceState_Gathering = 1, /**< Gathering local candidates. host/srflx/relay */
  xIceState_Checking  = 2, /**< Performing connectivity checks.              */
  xIceState_Connected = 3, /**< At least one valid pair found.               */
  xIceState_Completed = 4, /**< All checks done, nominated pair selected.    */
  xIceState_Failed    = 5, /**< All checks failed, no valid pair.            */
  xIceState_Closed    = 6, /**< Agent has been shut down.                    */
};

/* ───────────────────── Agent Roles ───────────────────── */

XDEF_ENUM(xIceRole){
  xIceRole_Controlling = 0,
  xIceRole_Controlled  = 1,
};

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when the agent state changes.
 * @param agent  The agent handle.
 * @param state  New state.
 * @param arg    User-provided argument.
 */
typedef void (*xIceOnStateChange)(xIceAgent agent, xIceState state, void *arg);

/**
 * @brief Called when a new local candidate is gathered.
 *
 * When @p candidate_sdp is NULL, gathering is complete.
 *
 * @param agent          The agent handle.
 * @param candidate_sdp  SDP candidate line (e.g. "candidate:..."), or NULL.
 * @param arg            User-provided argument.
 */
typedef void (*xIceOnCandidate)(xIceAgent agent, const char *candidate_sdp,
                                void *arg);

/**
 * @brief Called when application data is received on the nominated pair.
 * @param agent  The agent handle.
 * @param data   Received data buffer.
 * @param len    Length of data in bytes.
 * @param arg    User-provided argument.
 */
typedef void (*xIceOnData)(xIceAgent agent, const uint8_t *data, size_t len,
                           void *arg);

/* ───────────────────── Configuration ───────────────────── */

/**
 * @brief Configuration for creating an xIceAgent.
 */
XDEF_STRUCT(xIceConf) {
  xIceRole role;        /**< ICE role (Controlling or Controlled).   */
  bool     enable_ipv6; /**< Enable IPv6 candidates (default: false). */

  /** STUN server(s) (optional, "host:port" or comma-separated list
   *  "host1:port1,host2:port2" for port prediction, or NULL).
   *  When multiple servers are provided, the agent sends STUN Binding
   *  Requests to each server from the same socket to detect symmetric
   *  NAT port allocation patterns and generate predicted candidates. */
  const char *stun_server;

  /** Number of predicted port candidates to generate per host when
   *  symmetric NAT is detected (0 = use default of 5, max 10). */
  int port_predict_count;

  /** TURN server (optional, "host:port" or NULL). */
  const char *turn_server;
  const char *turn_username; /**< TURN long-term credential username.     */
  const char *turn_password; /**< TURN long-term credential password.     */

  /** Callbacks. */
  xIceOnStateChange on_state_change;
  xIceOnCandidate   on_candidate;
  xIceOnData        on_data;
  void             *ctx; /**< Forwarded to all callbacks.               */
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a new P2P agent.
 *
 * Generates random ice-ufrag and ice-pwd. Initial state is New.
 *
 * @param loop  Event loop (required, must not be NULL).
 * @param conf  Configuration (must not be NULL).
 * @return      Agent handle, or NULL on failure.
 */
XCAPI(xIceAgent) xIceAgentCreate(xEventLoop loop, const xIceConf *conf);

/**
 * @brief Destroy a P2P agent and release all resources.
 *
 * Closes sockets, cancels timers, frees memory. State becomes Closed.
 * Safe to call with NULL (no-op).
 *
 * @param agent  Agent handle, or NULL.
 */
XCAPI(void) xIceAgentDestroy(xIceAgent agent);

/* ───────────────────── Gathering ───────────────────── */

/**
 * @brief Start candidate gathering.
 *
 * Enumerates local interfaces, sends STUN Binding / TURN Allocate
 * requests. Candidates are reported via xIceOnCandidate callback.
 *
 * @param agent  Agent handle.
 * @return       xErrno_Ok on success.
 */
XCAPI(xErrno) xIceAgentGather(xIceAgent agent);

/* ───────────────────── SDP ───────────────────── */

/**
 * @brief Generate an SDP offer string.
 *
 * Caller must free the returned string with free().
 *
 * @param agent  Agent handle.
 * @return       Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xIceAgentCreateOffer(xIceAgent agent);

/**
 * @brief Generate an SDP answer string.
 *
 * Caller must free the returned string with free().
 *
 * @param agent  Agent handle.
 * @return       Heap-allocated SDP string, or NULL on failure.
 */
XCAPI(char *) xIceAgentCreateAnswer(xIceAgent agent);

/**
 * @brief Set the remote SDP description.
 *
 * Parses ice-ufrag, ice-pwd, and candidate lines from the SDP.
 *
 * @param agent  Agent handle.
 * @param sdp    Remote SDP string.
 * @return       xErrno_Ok on success.
 */
XCAPI(xErrno) xIceAgentSetRemoteDescription(xIceAgent agent, const char *sdp);

/**
 * @brief Add a remote candidate (Trickle ICE).
 *
 * @param agent          Agent handle.
 * @param candidate_sdp  SDP candidate line (e.g. "candidate:...").
 * @return               xErrno_Ok on success.
 */
XCAPI(xErrno) xIceAgentAddRemoteCandidate(xIceAgent   agent,
                                          const char *candidate_sdp);

/* ───────────────────── Data ───────────────────── */

/**
 * @brief Send data through the nominated pair.
 *
 * Only valid when the agent is in Connected or Completed state.
 *
 * @param agent  Agent handle.
 * @param data   Data to send.
 * @param len    Length of data in bytes.
 * @return       xErrno_Ok on success, xErrno_InvalidState if not connected.
 */
XCAPI(xErrno) xIceAgentSend(xIceAgent agent, const uint8_t *data, size_t len);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Get the local ice-ufrag.
 * @param agent  Agent handle.
 * @return       Pointer to internal ufrag string (valid while agent lives).
 */
XCAPI(const char *) xIceAgentGetUfrag(xIceAgent agent);

/**
 * @brief Get the local ice-pwd.
 * @param agent  Agent handle.
 * @return       Pointer to internal pwd string (valid while agent lives).
 */
XCAPI(const char *) xIceAgentGetPwd(xIceAgent agent);

/**
 * @brief Get the event loop associated with this agent.
 * @param agent  Agent handle.
 * @return       Event loop handle.
 */
XCAPI(xEventLoop) xIceAgentGetLoop(xIceAgent agent);

/* ───────────────────── Candidate Access ───────────────────── */

/**
 * @brief Get the array of gathered local candidates.
 *
 * Returns a pointer to the internal candidate array. The pointer is
 * valid as long as the agent is alive.
 *
 * @param agent      Agent handle.
 * @param out_count  Output: number of candidates (may be NULL).
 * @return           Pointer to internal candidate array, or NULL.
 */
XCAPI(const xIceCandidate *) xIceAgentGetLocalCandidates(xIceAgent agent,
                                                         int      *out_count);

/* ───────────────────── DTLS Input Hook ───────────────────── */

struct sockaddr; /* Forward declaration for callback signature */

/**
 * @brief Callback type for feeding DTLS data into an upper layer.
 *
 * When the ICE agent's UDP demux identifies a DTLS packet (RFC 7983),
 * it invokes this callback so the PeerConnection layer can feed the
 * data into its DTLS transport.
 */
typedef void (*xIceDtlsInputFn)(const uint8_t *data, size_t len,
                                const struct sockaddr *from, void *arg);

/**
 * @brief Register a DTLS input callback on the ICE agent.
 *
 * The ICE agent will call @p fn whenever a DTLS-range packet arrives
 * on the nominated pair's socket (RFC 7983 first-byte demux).
 *
 * @param agent  Agent handle.
 * @param fn     Callback function, or NULL to unregister.
 * @param arg    User-provided context forwarded to @p fn.
 */
XCAPI(void) xIceAgentSetDtlsInputCallback(xIceAgent agent,
                                           xIceDtlsInputFn fn, void *arg);

/**
 * @brief Change the ICE agent role (Controlling / Controlled).
 *
 * Must be called before connectivity checks start (i.e. before
 * SetRemoteDescription triggers checks).
 *
 * @param agent  Agent handle.
 * @param role   New role.
 */
XCAPI(void) xIceAgentSetRole(xIceAgent agent, xIceRole role);

#endif /* XP2P_ICE_AGENT_H */
