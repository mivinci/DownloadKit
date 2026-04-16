/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_private.h - Internal shared definitions for xIce module
 *
 * Contains STUN/TURN protocol constants, message type enums,
 * attribute type enums, ICE candidate/agent state enums, and
 * internal utility macros.
 */

#ifndef XP2P_ICE_PRIVATE_H
#define XP2P_ICE_PRIVATE_H

#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/socket.h>

#include <stdint.h>
#include <string.h>

/* ───────────────────── STUN Protocol Constants ───────────────────── */

/** STUN magic cookie (RFC 5389 §6). */
#define XSTUN_MAGIC_COOKIE 0x2112A442u

/** STUN header size in bytes (type + length + magic cookie + txn_id). */
#define XSTUN_HEADER_SIZE 20

/** STUN transaction ID size in bytes. */
#define XSTUN_TXN_ID_SIZE 12

/** STUN attribute header size (type + length). */
#define XSTUN_ATTR_HEADER_SIZE 4

/** SHA-1 digest size in bytes (MESSAGE-INTEGRITY). */
#define XSTUN_SHA1_DIGEST_SIZE 20

/** CRC-32 XOR mask for FINGERPRINT attribute. */
#define XSTUN_FINGERPRINT_XOR 0x5354554Eu

/** Maximum STUN message size (RFC 5389 recommends path MTU, we use 1280). */
#define XSTUN_MAX_MSG_SIZE 1280

/* ───────────────────── STUN Message Types ───────────────────── */

/**
 * STUN message type encoding (RFC 5389 §6):
 *   Bits: 0b00MMMM_MMMM_CCCC
 *   M = method bits, C = class bits
 *
 * Class:
 *   0b00 = Request
 *   0b01 = Indication
 *   0b10 = Success Response
 *   0b11 = Error Response
 */
XDEF_ENUM(xStunMsgType){
  /* STUN Binding (method 0x001) */
  xStunMsgType_BindingRequest       = 0x0001,
  xStunMsgType_BindingIndication    = 0x0011,
  xStunMsgType_BindingResponse      = 0x0101,
  xStunMsgType_BindingErrorResponse = 0x0111,

  /* TURN Allocate (method 0x003) */
  xStunMsgType_AllocateRequest       = 0x0003,
  xStunMsgType_AllocateResponse      = 0x0103,
  xStunMsgType_AllocateErrorResponse = 0x0113,

  /* TURN Refresh (method 0x004) */
  xStunMsgType_RefreshRequest       = 0x0004,
  xStunMsgType_RefreshResponse      = 0x0104,
  xStunMsgType_RefreshErrorResponse = 0x0114,

  /* TURN Send (method 0x006) */
  xStunMsgType_SendIndication = 0x0016,

  /* TURN Data (method 0x007) */
  xStunMsgType_DataIndication = 0x0017,

  /* TURN CreatePermission (method 0x008) */
  xStunMsgType_CreatePermissionRequest       = 0x0008,
  xStunMsgType_CreatePermissionResponse      = 0x0108,
  xStunMsgType_CreatePermissionErrorResponse = 0x0118,

  /* TURN ChannelBind (method 0x009) */
  xStunMsgType_ChannelBindRequest       = 0x0009,
  xStunMsgType_ChannelBindResponse      = 0x0109,
  xStunMsgType_ChannelBindErrorResponse = 0x0119,
};

/** Extract the STUN method from a message type. */
#define XSTUN_MSG_METHOD(type) \
  (((type) & 0x000F) | (((type) & 0x00E0) >> 1) | (((type) & 0x3E00) >> 2))

/** Extract the STUN class from a message type. */
#define XSTUN_MSG_CLASS(type) \
  ((((type) & 0x0010) >> 4) | (((type) & 0x0100) >> 7))

/** STUN message class values. */
#define XSTUN_CLASS_REQUEST      0x00
#define XSTUN_CLASS_INDICATION   0x01
#define XSTUN_CLASS_SUCCESS_RESP 0x02
#define XSTUN_CLASS_ERROR_RESP   0x03

/* ───────────────────── STUN Attribute Types ───────────────────── */

XDEF_ENUM(xStunAttrType){
  /* Comprehension-required (0x0000 - 0x7FFF) */
  xStunAttrType_MappedAddress     = 0x0001,
  xStunAttrType_Username          = 0x0006,
  xStunAttrType_MessageIntegrity  = 0x0008,
  xStunAttrType_ErrorCode         = 0x0009,
  xStunAttrType_UnknownAttributes = 0x000A,
  xStunAttrType_Realm             = 0x0014,
  xStunAttrType_Nonce             = 0x0015,
  xStunAttrType_XorMappedAddress  = 0x0020,

  /* ICE attributes */
  xStunAttrType_Priority     = 0x0024,
  xStunAttrType_UseCandidate = 0x0025,

  /* Comprehension-optional (0x8000 - 0xFFFF) */
  xStunAttrType_Software        = 0x8022,
  xStunAttrType_AlternateServer = 0x8023,
  xStunAttrType_Fingerprint     = 0x8028,

  /* ICE attributes (comprehension-optional range) */
  xStunAttrType_IceControlled  = 0x8029,
  xStunAttrType_IceControlling = 0x802A,

  /* TURN attributes */
  xStunAttrType_ChannelNumber      = 0x000C,
  xStunAttrType_Lifetime           = 0x000D,
  xStunAttrType_XorPeerAddress     = 0x0012,
  xStunAttrType_Data               = 0x0013,
  xStunAttrType_XorRelayedAddress  = 0x0016,
  xStunAttrType_RequestedTransport = 0x0019,
};

/* ───────────────────── ICE Candidate Types ───────────────────── */

XDEF_ENUM(xIceCandidateType){
  xIceCandidateType_Host  = 0,
  xIceCandidateType_Srflx = 1,
  xIceCandidateType_Prflx = 2,
  xIceCandidateType_Relay = 3,
};

/** Type preference values for priority calculation (RFC 8445 §5.1.2.1). */
#define XICE_TYPE_PREF_HOST  126
#define XICE_TYPE_PREF_SRFLX 100
#define XICE_TYPE_PREF_PRFLX 110
#define XICE_TYPE_PREF_RELAY 5

/* ───────────────────── ICE Candidate Pair States ───────────────────── */

XDEF_ENUM(xIcePairState){
  xIcePairState_Frozen = 0,     xIcePairState_Waiting = 1,
  xIcePairState_InProgress = 2, xIcePairState_Succeeded = 3,
  xIcePairState_Failed = 4,
};

/* ───────────────────── ICE Agent States ───────────────────── */

XDEF_ENUM(xIceAgentState){
  xIceAgentState_New = 0,       xIceAgentState_Gathering = 1,
  xIceAgentState_Checking = 2,  xIceAgentState_Connected = 3,
  xIceAgentState_Completed = 4, xIceAgentState_Failed = 5,
  xIceAgentState_Closed = 6,
};

/* ───────────────────── ICE Agent Roles ───────────────────── */

XDEF_ENUM(xIceAgentRole){
  xIceAgentRole_Controlling = 0,
  xIceAgentRole_Controlled  = 1,
};

/* ───────────────────── ICE Default Timeouts ───────────────────── */

/** Candidate gathering timeout in milliseconds. */
#define XICE_GATHER_TIMEOUT_MS 5000

/** Connectivity check timeout in milliseconds. */
#define XICE_CHECK_TIMEOUT_MS 10000

/** Connectivity check pacing interval in milliseconds. */
#define XICE_CHECK_PACING_MS 50

/** Consent freshness interval in milliseconds (RFC 7675). */
#define XICE_CONSENT_INTERVAL_MS 15000

/** Consent freshness failure timeout in milliseconds. */
#define XICE_CONSENT_TIMEOUT_MS 30000

/* ───────────────────── STUN Transaction Defaults ───────────────────── */

/** Initial retransmission timeout in milliseconds (RFC 5389 §7.2.1). */
#define XSTUN_INITIAL_RTO_MS 500

/** Maximum number of retransmissions. */
#define XSTUN_MAX_RETRANSMITS 7

/* ───────────────────── TURN Constants ───────────────────── */

/** TURN requested transport: UDP (0x11 = 17 = IPPROTO_UDP). */
#define XTURN_TRANSPORT_UDP 0x11000000u

/** TURN ChannelData number range. */
#define XTURN_CHANNEL_MIN 0x4000
#define XTURN_CHANNEL_MAX 0x7FFF

/** TURN ChannelData header size (channel number + length). */
#define XTURN_CHANNEL_HEADER_SIZE 4

/** TURN allocation refresh at 80% of lifetime. */
#define XTURN_REFRESH_RATIO 80

/* ───────────────────── Birthday Attack Constants ───────────────────── */

/** Default number of local sockets for birthday attack. */
#define XICE_BIRTHDAY_DEFAULT_K 32

/** Default number of random remote ports per socket. */
#define XICE_BIRTHDAY_DEFAULT_N 256

/** Maximum allowed local sockets (hard cap). */
#define XICE_BIRTHDAY_MAX_K 64

/** Maximum allowed remote ports per socket (hard cap). */
#define XICE_BIRTHDAY_MAX_N 512

/** Birthday attack overall timeout in milliseconds. */
#define XICE_BIRTHDAY_TIMEOUT_MS 10000

/** Birthday attack pacing interval in milliseconds (per burst). */
#define XICE_BIRTHDAY_PACING_MS 5

/** Minimum ephemeral port for birthday probing. */
#define XICE_BIRTHDAY_PORT_MIN 1024

/** Maximum ephemeral port for birthday probing. */
#define XICE_BIRTHDAY_PORT_MAX 65535

/* ───────────────────── ICE Credential Sizes ───────────────────── */

/** Minimum ice-ufrag length (RFC 8445 §16.1). */
#define XICE_UFRAG_MIN_LEN 4

/** Minimum ice-pwd length (RFC 8445 §16.1). */
#define XICE_PWD_MIN_LEN 22

/** Maximum ice-ufrag length we generate. */
#define XICE_UFRAG_LEN 8

/** Maximum ice-pwd length we generate. */
#define XICE_PWD_LEN 24

/** Maximum candidate foundation length. */
#define XICE_FOUNDATION_MAX_LEN 32
#define XICE_UFRAG_MAX_LEN      64
#define XICE_PWD_MAX_LEN        128

/* ───────────────────── Utility Macros ───────────────────── */

/** Align a value up to the next multiple of 4. */
#define XSTUN_ALIGN4(n) (((n) + 3) & ~3)

/** Read a 16-bit big-endian value from a byte pointer. */
static inline uint16_t xReadU16BE(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/** Read a 32-bit big-endian value from a byte pointer. */
static inline uint32_t xReadU32BE(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         (uint32_t)p[3];
}

/** Read a 64-bit big-endian value from a byte pointer. */
static inline uint64_t xReadU64BE(const uint8_t *p) {
  return (uint64_t)xReadU32BE(p) << 32 | xReadU32BE(p + 4);
}

/** Write a 16-bit big-endian value to a byte pointer. */
static inline void xWriteU16BE(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v);
}

/** Write a 32-bit big-endian value to a byte pointer. */
static inline void xWriteU32BE(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v);
}

/** Write a 64-bit big-endian value to a byte pointer. */
static inline void xWriteU64BE(uint8_t *p, uint64_t v) {
  xWriteU32BE(p, (uint32_t)(v >> 32));
  xWriteU32BE(p + 4, (uint32_t)(v));
}

/* ───────────────────── RFC 7983 Demux (first-byte ranges) ───────────── */

/**
 * @brief Classify a UDP packet by its first byte per RFC 7983.
 *
 * Returns:
 *   0 = STUN      [0, 3]
 *   1 = DTLS      [20, 63]
 *   2 = TURN ChannelData [64, 79]  (0x40-0x4F, subset of [64,79])
 *   3 = RTP/RTCP  [128, 191]  (reserved, not implemented)
 *  -1 = Unknown / discard
 */
static inline int xIceDemuxClassify(uint8_t first_byte) {
  if (first_byte <= 3) return 0;                              /* STUN */
  if (first_byte >= 20 && first_byte <= 63) return 1;         /* DTLS */
  if (first_byte >= 64 && first_byte <= 79) return 2;         /* TURN ChannelData */
  if (first_byte >= 128 && first_byte <= 191) return 3;       /* RTP/RTCP (reserved) */
  return -1;                                                  /* Unknown */
}

#define XICE_DEMUX_STUN          0
#define XICE_DEMUX_DTLS          1
#define XICE_DEMUX_TURN_CHANNEL  2
#define XICE_DEMUX_RTP           3

#endif /* XP2P_ICE_PRIVATE_H */
