/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_candidate.h - ICE candidate representation and priority
 */

#ifndef XP2P_ICE_CANDIDATE_H
#define XP2P_ICE_CANDIDATE_H

#include "ice_private.h"

#include <netinet/in.h>

/** Maximum number of candidates per agent.
 *  Increased to 512 to accommodate birthday-attack spray candidates
 *  generated during symmetric NAT traversal. */
#define XICE_MAX_CANDIDATES 512

/**
 * @brief An ICE candidate.
 */
XDEF_STRUCT(xIceCandidate) {
  char     foundation[XICE_FOUNDATION_MAX_LEN]; /**< Foundation string.     */
  int      component_id;                        /**< Component ID (1-256). */
  int      transport;                           /**< 0 = UDP.              */
  uint32_t priority;                            /**< Computed priority.    */
  xIceCandidateType type;                       /**< host/srflx/prflx/relay */

  struct sockaddr_storage addr;      /**< Candidate address.                  */
  struct sockaddr_storage base_addr; /**< Base address (for srflx/prflx).    */
  struct sockaddr_storage rel_addr;  /**< Related address (raddr/rport).     */

  xSocket sock; /**< Associated socket (for local cands).*/
};

/**
 * @brief Compute ICE candidate priority (RFC 8445 §5.1.2.1).
 *
 * priority = (2^24) * type_pref + (2^8) * local_pref + (256 - component_id)
 *
 * @param type          Candidate type.
 * @param local_pref    Local preference (0-65535).
 * @param component_id  Component ID (1-256).
 * @return              Computed priority.
 */
uint32_t xIceCandidatePriority(xIceCandidateType type, uint16_t local_pref,
                               int component_id);

/**
 * @brief Get the type preference for a candidate type.
 */
int xIceCandidateTypePref(xIceCandidateType type);

/**
 * @brief Generate a foundation string for a candidate.
 *
 * Same type + base address + STUN server → same foundation.
 *
 * @param cand         Candidate to generate foundation for.
 * @param stun_server  STUN server address (may be NULL for host candidates).
 */
void xIceCandidateFoundation(xIceCandidate         *cand,
                             const struct sockaddr *stun_server);

/**
 * @brief Get the candidate type as a string (for SDP).
 */
const char *xIceCandidateTypeStr(xIceCandidateType type);

/**
 * @brief Parse a candidate type string (from SDP).
 *
 * @param str   Type string ("host", "srflx", "prflx", "relay").
 * @param type  Output type.
 * @return      xErrno_Ok on success.
 */
xErrno xIceCandidateTypeFromStr(const char *str, xIceCandidateType *type);

/**
 * @brief Get the port from a sockaddr.
 */
uint16_t xSockaddrPort(const struct sockaddr *addr);

/**
 * @brief Get the IP address string from a sockaddr.
 *
 * @param addr  Socket address.
 * @param buf   Output buffer.
 * @param len   Buffer length.
 * @return      buf on success, NULL on failure.
 */
const char *xSockaddrIP(const struct sockaddr *addr, char *buf, size_t len);

#endif /* XP2P_ICE_CANDIDATE_H */
