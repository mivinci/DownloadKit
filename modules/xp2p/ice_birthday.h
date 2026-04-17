/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_birthday.h - NAT detection and birthday attack internal interfaces
 *
 * This module implements:
 *   1. NAT type detection during ICE gathering
 *   2. Signaling-coordinated birthday attack state machine
 *   3. Optimized probing strategies based on NAT type
 */

#ifndef XP2P_ICE_BIRTHDAY_H
#define XP2P_ICE_BIRTHDAY_H

#include "ice_agent.h"
#include "ice_agent_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Birthday Attack States ───────────────────── */

/**
 * @brief Internal state of the birthday attack state machine.
 */
XDEF_ENUM(xIceBirthdayState){
  xIceBirthdayState_Idle      = 0, /**< Not started.                       */
  xIceBirthdayState_WaitInit  = 1, /**< Controlled: waiting for Init msg.  */
  xIceBirthdayState_WaitReady = 2, /**< Controlling: waiting for Ready.    */
  xIceBirthdayState_WaitGo    = 3, /**< Controlled: waiting for Go msg.    */
  xIceBirthdayState_Probing   = 4, /**< Both sides: actively probing.      */
  xIceBirthdayState_Done      = 5, /**< Attack finished (hit or timeout).  */
};

/* ───────────────────── Birthday Attack Context ───────────────────── */

/**
 * @brief Birthday attack context.
 *
 * Holds all state for a single birthday attack session, including
 * sockets, timers, target address, and probing parameters.
 */
typedef struct xIceBirthdayCtx {
  xIceAgent_       *agent;          /**< Back-pointer to owning agent.     */
  xIceBirthdayState state;          /**< Current state machine state.      */

  /* Parameters */
  int      k;                       /**< Number of local sockets.          */
  int      n;                       /**< Random remote ports per socket.   */
  int      timeout_ms;              /**< Overall timeout in ms.            */

  /* Sockets */
  xSocket *socks;                   /**< Heap-allocated socket array.      */
  int      sock_count;              /**< Actual created sockets.           */

  /* Probing state */
  int      burst_index;             /**< Current burst index.              */
  uint16_t target_port;             /**< Remote srflx port (center).       */
  struct sockaddr_storage target;   /**< Remote NAT public IP.             */

  /* Timers */
  xEventTimer pacing_timer;         /**< Pacing timer for bursts.          */
  xEventTimer timeout_timer;        /**< Overall timeout timer.            */
} xIceBirthdayCtx;

/* ───────────────────── NAT Detection ───────────────────── */

/**
 * @brief NAT detection context for gathering phase.
 *
 * Uses multiple sockets to send STUN Binding Requests to the STUN
 * server and collects the mapped ports to determine NAT type.
 */
#define XICE_NAT_DETECT_SOCKETS 3

typedef struct xIceNatDetectCtx {
  xIceAgent_ *agent;                              /**< Back-pointer.       */
  uint16_t    mapped_ports[XICE_NAT_DETECT_SOCKETS]; /**< Collected ports. */
  int         responses;                           /**< Responses received. */
  int         pending;                             /**< Pending requests.   */
  xEventTimer timer;                               /**< Detection timeout.  */
  bool        done;                                /**< Already completed.  */
} xIceNatDetectCtx;

/**
 * @brief Start NAT type detection during gathering.
 *
 * Sends STUN Binding Requests from multiple sockets to the STUN server
 * and analyzes the mapped ports to determine the NAT type.
 *
 * @param agent     Internal agent pointer.
 * @param stun_addr Resolved STUN server address.
 * @return          xErrno_Ok if detection started, error otherwise.
 */
xErrno xIceBirthdayDetectNat(xIceAgent_                    *agent,
                             const struct sockaddr_storage *stun_addr);

/**
 * @brief Classify NAT type from collected mapped ports.
 *
 * @param ports      Array of mapped ports.
 * @param count      Number of valid ports.
 * @return           Detected NAT type.
 */
xNatType xIceBirthdayClassifyNat(const uint16_t *ports, int count);

/* ───────────────────── Birthday Attack Lifecycle ───────────────────── */

/**
 * @brief Initialize a birthday attack context.
 *
 * @param ctx    Context to initialize.
 * @param agent  Internal agent pointer.
 * @param k      Number of local sockets.
 * @param n      Random remote ports per socket.
 * @param timeout_ms  Overall timeout in ms.
 */
void xIceBirthdayInit(xIceBirthdayCtx *ctx, xIceAgent_ *agent,
                      int k, int n, int timeout_ms);

/**
 * @brief Handle a birthday signal message.
 *
 * Drives the birthday attack state machine based on the received
 * signal type.
 *
 * @param ctx     Birthday context.
 * @param signal  The received signal message.
 * @return        xErrno_Ok on success.
 */
xErrno xIceBirthdayHandleSignal(xIceBirthdayCtx          *ctx,
                                const xIceBirthdaySignal *signal);

/**
 * @brief Start the birthday attack as the controlling (initiating) side.
 *
 * Sends birthday_init through the signaling callback and transitions
 * to WaitReady state.
 *
 * @param ctx  Birthday context.
 * @return     xErrno_Ok on success.
 */
xErrno xIceBirthdayStartAsController(xIceBirthdayCtx *ctx);

/**
 * @brief Clean up all birthday attack resources.
 *
 * Cancels timers, destroys sockets, resets state.
 *
 * @param ctx  Birthday context.
 */
void xIceBirthdayCleanup(xIceBirthdayCtx *ctx);

/**
 * @brief Build a lightweight STUN Binding Request probe (20 bytes).
 *
 * @param buf  Output buffer (must be >= XSTUN_HEADER_SIZE).
 * @return     Encoded length (always XSTUN_HEADER_SIZE).
 */
size_t xIceBirthdayBuildProbe(uint8_t *buf);

#endif /* XP2P_ICE_BIRTHDAY_H */
