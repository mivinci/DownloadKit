/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sdp.c - SDP encoding / decoding for ICE (RFC 4566)
 */

#include "sdp.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Candidate Line ───────────────────── */

int xIceSdpEncodeCandidate(const xIceCandidate *cand, char *out, size_t cap) {
  if (!cand || !out) return -1;

  char addr_str[INET6_ADDRSTRLEN];
  if (!xSockaddrIP((const struct sockaddr *)&cand->addr, addr_str,
                   sizeof(addr_str))) {
    return -1;
  }
  uint16_t    port     = xSockaddrPort((const struct sockaddr *)&cand->addr);
  const char *type_str = xIceCandidateTypeStr(cand->type);

  int written;
  if (cand->type == xIceCandidateType_Srflx ||
      cand->type == xIceCandidateType_Prflx ||
      cand->type == xIceCandidateType_Relay) {
    char     raddr_str[INET6_ADDRSTRLEN];
    uint16_t rport = 0;
    if (cand->rel_addr.ss_family != 0) {
      xSockaddrIP((const struct sockaddr *)&cand->rel_addr, raddr_str,
                  sizeof(raddr_str));
      rport = xSockaddrPort((const struct sockaddr *)&cand->rel_addr);
    } else {
      strncpy(raddr_str, "0.0.0.0", sizeof(raddr_str));
    }
    written = snprintf(
      out, cap, "a=candidate:%s %d UDP %u %s %u typ %s raddr %s rport %u\r\n",
      cand->foundation, cand->component_id, cand->priority, addr_str, port,
      type_str, raddr_str, rport);
  } else {
    written = snprintf(out, cap, "a=candidate:%s %d UDP %u %s %u typ %s\r\n",
                       cand->foundation, cand->component_id, cand->priority,
                       addr_str, port, type_str);
  }

  if (written < 0 || (size_t)written >= cap) return -1;
  return written;
}

xErrno xIceSdpDecodeCandidate(const char *line, xIceCandidate *cand) {
  if (!line || !cand) return xErrno_InvalidArg;

  memset(cand, 0, sizeof(*cand));

  /* Skip "a=candidate:" prefix if present */
  const char *p = line;
  if (strncmp(p, "a=candidate:", 12) == 0) {
    p += 12;
  } else if (strncmp(p, "candidate:", 10) == 0) {
    p += 10;
  }

  /* Parse: foundation component transport priority address port */
  char         foundation[XICE_FOUNDATION_MAX_LEN];
  int          component;
  char         transport[8];
  unsigned int priority;
  char         addr_str[INET6_ADDRSTRLEN];
  unsigned int port;

  int consumed = 0;
  int n        = sscanf(p, "%31s %d %7s %u %45s %u%n", foundation, &component,
                        transport, &priority, addr_str, &port, &consumed);
  if (n < 6) return xErrno_InvalidArg;

  strncpy(cand->foundation, foundation, XICE_FOUNDATION_MAX_LEN - 1);
  cand->component_id = component;
  cand->priority     = priority;

  /* Parse address */
  struct sockaddr_in  *a4 = (struct sockaddr_in *)&cand->addr;
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&cand->addr;
  if (inet_pton(AF_INET, addr_str, &a4->sin_addr) == 1) {
    a4->sin_family = AF_INET;
    a4->sin_port   = htons((uint16_t)port);
  } else if (inet_pton(AF_INET6, addr_str, &a6->sin6_addr) == 1) {
    a6->sin6_family = AF_INET6;
    a6->sin6_port   = htons((uint16_t)port);
  } else {
    return xErrno_InvalidArg;
  }

  /* Parse "typ <type>" */
  p += consumed;
  char type_str[16] = {0};
  if (sscanf(p, " typ %15s%n", type_str, &consumed) >= 1) {
    if (xIceCandidateTypeFromStr(type_str, &cand->type) != xErrno_Ok) {
      return xErrno_InvalidArg;
    }
    p += consumed;
  } else {
    return xErrno_InvalidArg;
  }

  /* Parse optional "raddr <addr> rport <port>" */
  char         raddr_str[INET6_ADDRSTRLEN] = {0};
  unsigned int rport                       = 0;
  if (sscanf(p, " raddr %45s rport %u", raddr_str, &rport) == 2) {
    struct sockaddr_in  *r4 = (struct sockaddr_in *)&cand->rel_addr;
    struct sockaddr_in6 *r6 = (struct sockaddr_in6 *)&cand->rel_addr;
    if (inet_pton(AF_INET, raddr_str, &r4->sin_addr) == 1) {
      r4->sin_family = AF_INET;
      r4->sin_port   = htons((uint16_t)rport);
    } else if (inet_pton(AF_INET6, raddr_str, &r6->sin6_addr) == 1) {
      r6->sin6_family = AF_INET6;
      r6->sin6_port   = htons((uint16_t)rport);
    }
  }

  return xErrno_Ok;
}

/* ───────────────────── Full SDP ───────────────────── */

int xIceSdpEncode(const char *ufrag, const char *pwd,
                  const xIceCandidate *candidates, int cand_count, bool trickle,
                  char *out, size_t out_cap) {
  if (!ufrag || !pwd || !out) return -1;

  int pos = 0;
  int n;

  /* Session-level lines */
  n = snprintf(out + pos, out_cap - pos,
               "v=0\r\n"
               "o=- 0 0 IN IP4 0.0.0.0\r\n"
               "s=-\r\n"
               "t=0 0\r\n"
               "m=application 9 UDP/ICE 0\r\n"
               "c=IN IP4 0.0.0.0\r\n"
               "a=ice-ufrag:%s\r\n"
               "a=ice-pwd:%s\r\n",
               ufrag, pwd);
  if (n < 0 || pos + n >= (int)out_cap) return -1;
  pos += n;

  if (trickle) {
    n = snprintf(out + pos, out_cap - pos, "a=ice-options:trickle\r\n");
    if (n < 0 || pos + n >= (int)out_cap) return -1;
    pos += n;
  }

  /* Candidate lines */
  for (int i = 0; i < cand_count; i++) {
    n = xIceSdpEncodeCandidate(&candidates[i], out + pos, out_cap - pos);
    if (n < 0) return -1;
    pos += n;
  }

  return pos;
}

/* ───────────────────── WebRTC SDP Encoding ───────────────────── */

static const char *setup_to_str(xIceSdpSetup setup) {
  switch (setup) {
  case xIceSdpSetup_Active:  return "active";
  case xIceSdpSetup_Passive: return "passive";
  case xIceSdpSetup_Actpass: return "actpass";
  default:                   return "actpass";
  }
}

int xIceSdpEncodeWebRTC(const char *ufrag, const char *pwd,
                        const xIceCandidate *candidates, int cand_count,
                        bool trickle, const char *fingerprint,
                        xIceSdpSetup setup, const char *mid,
                        uint16_t sctp_port, char *out, size_t out_cap) {
  if (!ufrag || !pwd || !fingerprint || !mid || !out) return -1;

  int pos = 0;
  int n;

  /* Session-level lines */
  n = snprintf(out + pos, out_cap - pos,
               "v=0\r\n"
               "o=- 0 0 IN IP4 0.0.0.0\r\n"
               "s=-\r\n"
               "t=0 0\r\n"
               "a=group:BUNDLE %s\r\n"
               "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
               "c=IN IP4 0.0.0.0\r\n"
               "a=mid:%s\r\n"
               "a=ice-ufrag:%s\r\n"
               "a=ice-pwd:%s\r\n"
               "a=fingerprint:%s\r\n"
               "a=setup:%s\r\n"
               "a=sctp-port:%u\r\n",
               mid, mid, ufrag, pwd, fingerprint,
               setup_to_str(setup), sctp_port);
  if (n < 0 || pos + n >= (int)out_cap) return -1;
  pos += n;

  if (trickle) {
    n = snprintf(out + pos, out_cap - pos, "a=ice-options:trickle\r\n");
    if (n < 0 || pos + n >= (int)out_cap) return -1;
    pos += n;
  }

  /* Candidate lines */
  for (int i = 0; i < cand_count; i++) {
    n = xIceSdpEncodeCandidate(&candidates[i], out + pos, out_cap - pos);
    if (n < 0) return -1;
    pos += n;
  }

  return pos;
}

/* ───────────────────── SDP Decoding ───────────────────── */

xErrno xIceSdpDecode(const char *sdp_str, size_t sdp_len, xIceSdp *out) {
  if (!sdp_str || !out) return xErrno_InvalidArg;

  memset(out, 0, sizeof(*out));

  /* Work with a null-terminated copy */
  char *buf = (char *)malloc(sdp_len + 1);
  if (!buf) return xErrno_NoMemory;
  memcpy(buf, sdp_str, sdp_len);
  buf[sdp_len] = '\0';

  bool got_ufrag = false;
  bool got_pwd   = false;

  char *line = buf;
  while (line && *line) {
    /* Find end of line */
    char *eol = strstr(line, "\r\n");
    if (eol) {
      *eol = '\0';
    }

    if (strncmp(line, "a=ice-ufrag:", 12) == 0) {
      strncpy(out->ice_ufrag, line + 12, XICE_UFRAG_MAX_LEN - 1);
      got_ufrag = true;
    } else if (strncmp(line, "a=ice-pwd:", 10) == 0) {
      strncpy(out->ice_pwd, line + 10, XICE_PWD_MAX_LEN - 1);
      got_pwd = true;
    } else if (strncmp(line, "a=ice-options:", 14) == 0) {
      if (strstr(line + 14, "trickle")) {
        out->trickle = true;
      }
    } else if (strcmp(line, "a=end-of-candidates") == 0) {
      out->end_of_candidates = true;
    } else if (strncmp(line, "a=candidate:", 12) == 0) {
      if (out->candidate_count < XICE_MAX_CANDIDATES) {
        xIceCandidate cand;
        if (xIceSdpDecodeCandidate(line, &cand) == xErrno_Ok) {
          out->candidates[out->candidate_count++] = cand;
        }
      }
    } else if (strncmp(line, "a=fingerprint:", 14) == 0) {
      strncpy(out->fingerprint, line + 14, XSDP_MAX_FINGERPRINT_LEN - 1);
    } else if (strncmp(line, "a=setup:", 8) == 0) {
      const char *val = line + 8;
      if (strcmp(val, "active") == 0) {
        out->setup = xIceSdpSetup_Active;
      } else if (strcmp(val, "passive") == 0) {
        out->setup = xIceSdpSetup_Passive;
      } else if (strcmp(val, "actpass") == 0) {
        out->setup = xIceSdpSetup_Actpass;
      }
    } else if (strncmp(line, "a=mid:", 6) == 0) {
      strncpy(out->mid, line + 6, XSDP_MAX_MID_LEN - 1);
    } else if (strncmp(line, "a=sctp-port:", 12) == 0) {
      out->sctp_port = (uint16_t)atoi(line + 12);
    } else if (strncmp(line, "m=application", 13) == 0) {
      /* Detect WebRTC media line format */
      if (strstr(line, "UDP/DTLS/SCTP") || strstr(line, "webrtc-datachannel")) {
        out->is_webrtc = true;
      }
    }

    if (eol) {
      line = eol + 2;
    } else {
      break;
    }
  }

  free(buf);

  if (!got_ufrag || !got_pwd) {
    return xErrno_InvalidArg;
  }

  /* For WebRTC SDP, fingerprint is required */
  if (out->is_webrtc && out->fingerprint[0] == '\0') {
    return xErrno_InvalidArg;
  }

  return xErrno_Ok;
}
