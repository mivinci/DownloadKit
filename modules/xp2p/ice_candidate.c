/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_candidate.c - ICE candidate priority and foundation
 */

#include "ice_candidate.h"

#include <xcrypto/crc32.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int xIceCandidateTypePref(xIceCandidateType type) {
  switch (type) {
  case xIceCandidateType_Host:
    return XICE_TYPE_PREF_HOST;
  case xIceCandidateType_Srflx:
    return XICE_TYPE_PREF_SRFLX;
  case xIceCandidateType_Prflx:
    return XICE_TYPE_PREF_PRFLX;
  case xIceCandidateType_Relay:
    return XICE_TYPE_PREF_RELAY;
  default:
    return 0;
  }
}

uint32_t xIceCandidatePriority(xIceCandidateType type, uint16_t local_pref,
                               int component_id) {
  int type_pref = xIceCandidateTypePref(type);
  return ((uint32_t)type_pref << 24) | ((uint32_t)local_pref << 8) |
         (uint32_t)(256 - component_id);
}

void xIceCandidateFoundation(xIceCandidate         *cand,
                             const struct sockaddr *stun_server) {
  /*
   * Foundation = hash(type, base_addr, stun_server).
   * We use CRC-32 of the concatenation for simplicity.
   */
  uint8_t buf[128];
  size_t  pos = 0;

  /* Type */
  buf[pos++] = (uint8_t)cand->type;

  /* Base address */
  if (cand->base_addr.ss_family == AF_INET) {
    struct sockaddr_in *a4 = (struct sockaddr_in *)&cand->base_addr;
    memcpy(buf + pos, &a4->sin_addr, 4);
    pos += 4;
  } else if (cand->base_addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&cand->base_addr;
    memcpy(buf + pos, &a6->sin6_addr, 16);
    pos += 16;
  }

  /* STUN server (if any) */
  if (stun_server) {
    if (stun_server->sa_family == AF_INET) {
      const struct sockaddr_in *s4 = (const struct sockaddr_in *)stun_server;
      memcpy(buf + pos, &s4->sin_addr, 4);
      pos += 4;
      memcpy(buf + pos, &s4->sin_port, 2);
      pos += 2;
    } else if (stun_server->sa_family == AF_INET6) {
      const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)stun_server;
      memcpy(buf + pos, &s6->sin6_addr, 16);
      pos += 16;
      memcpy(buf + pos, &s6->sin6_port, 2);
      pos += 2;
    }
  }

  uint32_t hash = xCrc32(buf, pos);
  snprintf(cand->foundation, XICE_FOUNDATION_MAX_LEN, "%u", hash);
}

const char *xIceCandidateTypeStr(xIceCandidateType type) {
  switch (type) {
  case xIceCandidateType_Host:
    return "host";
  case xIceCandidateType_Srflx:
    return "srflx";
  case xIceCandidateType_Prflx:
    return "prflx";
  case xIceCandidateType_Relay:
    return "relay";
  default:
    return "unknown";
  }
}

xErrno xIceCandidateTypeFromStr(const char *str, xIceCandidateType *type) {
  if (!str || !type) return xErrno_InvalidArg;
  if (strcmp(str, "host") == 0) {
    *type = xIceCandidateType_Host;
    return xErrno_Ok;
  }
  if (strcmp(str, "srflx") == 0) {
    *type = xIceCandidateType_Srflx;
    return xErrno_Ok;
  }
  if (strcmp(str, "prflx") == 0) {
    *type = xIceCandidateType_Prflx;
    return xErrno_Ok;
  }
  if (strcmp(str, "relay") == 0) {
    *type = xIceCandidateType_Relay;
    return xErrno_Ok;
  }
  return xErrno_InvalidArg;
}

uint16_t xSockaddrPort(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) {
    return ntohs(((const struct sockaddr_in *)addr)->sin_port);
  } else if (addr->sa_family == AF_INET6) {
    return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
  }
  return 0;
}

const char *xSockaddrIP(const struct sockaddr *addr, char *buf, size_t len) {
  if (addr->sa_family == AF_INET) {
    return inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr,
                     buf, (socklen_t)len);
  } else if (addr->sa_family == AF_INET6) {
    return inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)addr)->sin6_addr,
                     buf, (socklen_t)len);
  }
  return NULL;
}
