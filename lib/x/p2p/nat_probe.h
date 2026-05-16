/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nat_probe.h - NAT type detection via STUN (RFC 5389)
 *
 * Two-phase detection using two different STUN servers:
 *
 * Phase 1 — Cone vs Symmetric (1 socket → 2 STUN servers):
 *   socket0 → STUN server A → mapped_A (ip_a:port_a)
 *   socket0 → STUN server B → mapped_B (ip_b:port_b)
 *
 *   port_a == port_b  → Cone (NAT1/2/3, non-symmetric)
 *   port_a != port_b  → Symmetric → proceed to Phase 2
 *
 * Phase 2 — Sequential vs Random (3 sockets → STUN server A):
 *   socket1 → STUN server A → mapped1
 *   socket2 → STUN server A → mapped2
 *   socket3 → STUN server A → mapped3
 *
 *   deltas equal      → SymmetricSequential (predictable)
 *   deltas vary       → SymmetricRandom     (unpredictable)
 *
 * The probe is fully asynchronous and driven by the caller's xEventLoop.
 */

#ifndef XP2P_NAT_PROBE_H
#define XP2P_NAT_PROBE_H

#include <x/base/base.h>
#include <x/base/event.h>

/* ───────────────────── NAT Type ───────────────────── */

/**
 * @brief NAT type classification.
 */
XDEF_ENUM(xNatType){
  xNatType_Unknown             = 0, /**< Detection not yet complete or failed. */
  xNatType_OpenInternet        = 1, /**< No NAT; public IP directly reachable. */
  xNatType_Cone                = 2, /**< Full-cone / restricted-cone / port-restricted-cone. */
  xNatType_SymmetricRandom     = 3, /**< Symmetric NAT with random port allocation. */
  xNatType_SymmetricSequential = 4, /**< Symmetric NAT with sequential port allocation. */
};

/* ───────────────────── Probe Result ───────────────────── */

/**
 * @brief Result of a completed NAT probe.
 */
XDEF_STRUCT(xNatProbeResult) {
  xNatType type;         /**< Detected NAT type.                          */
  int      port_delta;   /**< Port step (>0 only for SymmetricSequential). */
  uint16_t mapped_ports[5]; /**< Phase1: [0..1], Phase2: [2..4].          */
};

/* ───────────────────── Probe Handle ───────────────────── */

/**
 * @brief Opaque handle to an in-progress NAT probe.
 */
XDEF_HANDLE(xNatProbe);

/* ───────────────────── Callback ───────────────────── */

/**
 * @brief Callback invoked when the NAT probe completes (or fails).
 *
 * @param result  Probe result. On failure, result->type == xNatType_Unknown.
 * @param arg     User-provided argument.
 */
typedef void (*xNatProbeFunc)(const xNatProbeResult *result, void *arg);

/* ───────────────────── API ───────────────────── */

/**
 * @brief Start an asynchronous NAT probe.
 *
 * Phase 1: Opens one UDP socket, sends STUN Binding Requests to two
 * different STUN servers, and compares the mapped ports to determine
 * if the NAT is Cone or Symmetric.
 *
 * Phase 2 (only if Symmetric): Opens three additional UDP sockets,
 * sends Binding Requests to the first STUN server, and analyzes port
 * allocation patterns (sequential vs random).
 *
 * @param loop         Event loop to drive the probe.
 * @param stun_host1   First STUN server hostname or IP string.
 * @param stun_port1   First STUN server port (typically 3478).
 * @param stun_host2   Second STUN server hostname or IP string.
 * @param stun_port2   Second STUN server port (typically 3478).
 * @param timeout_ms   Per-request timeout in milliseconds (e.g. 3000).
 * @param cb           Completion callback (must not be NULL).
 * @param arg          User argument forwarded to @p cb.
 * @return             A probe handle on success, or NULL on failure.
 *                     The handle is automatically freed after @p cb returns.
 */
XCAPI(xNatProbe) xNatProbeStart(xEventLoop loop,
                                const char *stun_host1, uint16_t stun_port1,
                                const char *stun_host2, uint16_t stun_port2,
                                int timeout_ms,
                                xNatProbeFunc cb, void *arg);

/**
 * @brief Cancel an in-progress NAT probe.
 *
 * Cancels all pending STUN transactions and frees the probe handle.
 * The completion callback will NOT be invoked.
 * Safe to call with NULL (no-op).
 *
 * @param probe  Probe handle returned by xNatProbeStart().
 */
XCAPI(void) xNatProbeCancel(xNatProbe probe);

/**
 * @brief Return a human-readable string for a NAT type.
 */
XCAPI(const char *) xNatTypeStr(xNatType type);

#endif /* XP2P_NAT_PROBE_H */
