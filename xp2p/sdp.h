/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sdp.h - SDP encoding / decoding for ICE (RFC 4566)
 */

#ifndef XP2P_SDP_H
#define XP2P_SDP_H

#include "ice_candidate.h"
#include "ice_private.h"

/** Maximum SDP string length. */
#define XSDP_MAX_SIZE 4096

/**
 * @brief Parsed SDP description.
 */
XDEF_STRUCT(xIceSdp) {
  char ice_ufrag[XICE_UFRAG_MAX_LEN];
  char ice_pwd[XICE_PWD_MAX_LEN];
  bool trickle;                          /**< ice-options:trickle present. */
  bool end_of_candidates;                /**< a=end-of-candidates present.*/

  xIceCandidate candidates[XICE_MAX_CANDIDATES];
  int candidate_count;
};

/**
 * @brief Encode an SDP offer/answer string.
 *
 * @param ufrag       Local ice-ufrag.
 * @param pwd         Local ice-pwd.
 * @param candidates  Array of local candidates.
 * @param cand_count  Number of candidates.
 * @param trickle     Whether to include ice-options:trickle.
 * @param out         Output buffer.
 * @param out_cap     Output buffer capacity.
 * @return            Length of encoded SDP, or -1 on error.
 */
int xIceSdpEncode(const char *ufrag, const char *pwd,
                   const xIceCandidate *candidates, int cand_count,
                   bool trickle, char *out, size_t out_cap);

/**
 * @brief Decode an SDP string.
 *
 * @param sdp_str  SDP string.
 * @param sdp_len  Length of SDP string.
 * @param out      Output parsed SDP.
 * @return         xErrno_Ok on success.
 */
xErrno xIceSdpDecode(const char *sdp_str, size_t sdp_len, xIceSdp *out);

/**
 * @brief Encode a single candidate line (for Trickle ICE).
 *
 * @param cand  Candidate to encode.
 * @param out   Output buffer.
 * @param cap   Buffer capacity.
 * @return      Length of encoded line, or -1 on error.
 */
int xIceSdpEncodeCandidate(const xIceCandidate *cand, char *out, size_t cap);

/**
 * @brief Decode a single candidate line (for Trickle ICE).
 *
 * @param line  Candidate line (with or without "a=candidate:" prefix).
 * @param cand  Output candidate.
 * @return      xErrno_Ok on success.
 */
xErrno xIceSdpDecodeCandidate(const char *line, xIceCandidate *cand);

#endif /* XP2P_SDP_H */
