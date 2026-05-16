/*
 * Copyright 2025 The libx Authors. All rights reserved.
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

/** Maximum fingerprint string length ("sha-256 AA:BB:CC:..."). */
#define XSDP_MAX_FINGERPRINT_LEN 128

/** Maximum setup role string length. */
#define XSDP_MAX_SETUP_LEN 16

/** Maximum mid string length. */
#define XSDP_MAX_MID_LEN 32

/* ───────────────────── SDP Setup Role ───────────────────── */

XDEF_ENUM(xIceSdpSetup){
  xIceSdpSetup_Active  = 0, /**< a=setup:active  */
  xIceSdpSetup_Passive = 1, /**< a=setup:passive */
  xIceSdpSetup_Actpass = 2, /**< a=setup:actpass */
};

/**
 * @brief Parsed SDP description.
 */
XDEF_STRUCT(xIceSdp) {
  char ice_ufrag[XICE_UFRAG_MAX_LEN];
  char ice_pwd[XICE_PWD_MAX_LEN];
  bool trickle;           /**< ice-options:trickle present. */
  bool end_of_candidates; /**< a=end-of-candidates present.*/

  xIceCandidate candidates[XICE_MAX_CANDIDATES];
  int           candidate_count;

  /* WebRTC extensions (populated when media line is UDP/DTLS/SCTP) */
  bool is_webrtc; /**< true if WebRTC SDP format detected.     */
  char
    fingerprint[XSDP_MAX_FINGERPRINT_LEN]; /**< a=fingerprint value (e.g. "sha-256 AA:BB:..."). */
  xIceSdpSetup setup;                      /**< a=setup role.                           */
  char         mid[XSDP_MAX_MID_LEN];      /**< a=mid value.                  */
  uint16_t     sctp_port;                  /**< a=sctp-port value (default 5000).       */
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
int xIceSdpEncode(const char *ufrag, const char *pwd, const xIceCandidate *candidates,
                  int cand_count, bool trickle, char *out, size_t out_cap);

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

/**
 * @brief Encode a WebRTC-compatible SDP (with DTLS/SCTP/DataChannel).
 *
 * Generates SDP with m=application 9 UDP/DTLS/SCTP webrtc-datachannel,
 * a=fingerprint, a=setup, a=mid, a=group:BUNDLE, a=sctp-port.
 *
 * @param ufrag        Local ice-ufrag.
 * @param pwd          Local ice-pwd.
 * @param candidates   Array of local candidates.
 * @param cand_count   Number of candidates.
 * @param trickle      Whether to include ice-options:trickle.
 * @param fingerprint  DTLS fingerprint string (e.g. "sha-256 AA:BB:...").
 * @param setup        DTLS setup role.
 * @param mid          Media ID (e.g. "0").
 * @param sctp_port    SCTP port (e.g. 5000).
 * @param out          Output buffer.
 * @param out_cap      Output buffer capacity.
 * @return             Length of encoded SDP, or -1 on error.
 */
int xIceSdpEncodeWebRTC(const char *ufrag, const char *pwd, const xIceCandidate *candidates,
                        int cand_count, bool trickle, const char *fingerprint, xIceSdpSetup setup,
                        const char *mid, uint16_t sctp_port, char *out, size_t out_cap);

#endif /* XP2P_SDP_H */
