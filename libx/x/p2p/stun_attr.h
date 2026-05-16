/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_attr.h - STUN attribute encoding / decoding (RFC 5389)
 */

#ifndef XP2P_STUN_ATTR_H
#define XP2P_STUN_ATTR_H

#include "ice_private.h"
#include "stun_msg.h"

#include <netinet/in.h>
#include <stdint.h>

/* ───────────────────── Attribute Writer ───────────────────── */

/**
 * @brief Cursor-based attribute writer.
 *
 * Tracks the current write position in a buffer so that multiple
 * attributes can be appended sequentially.
 */
XDEF_STRUCT(xStunAttrWriter) {
  uint8_t *buf; /**< Start of attribute area (after STUN header). */
  size_t   cap; /**< Capacity of attribute area.                  */
  size_t   pos; /**< Current write position.                      */
};

/**
 * @brief Initialize an attribute writer.
 *
 * @param w    Writer to initialize.
 * @param buf  Buffer for attribute data (typically msg_buf + 20).
 * @param cap  Capacity of the buffer.
 */
void xStunAttrWriterInit(xStunAttrWriter *w, uint8_t *buf, size_t cap);

/**
 * @brief Write a XOR-MAPPED-ADDRESS attribute.
 *
 * @param w       Attribute writer.
 * @param addr    Socket address (IPv4 or IPv6).
 * @param txn_id  Transaction ID (needed for IPv6 XOR).
 * @return        xErrno_Ok on success.
 */
xErrno xStunAttrWriteXorMappedAddress(xStunAttrWriter *w, const struct sockaddr *addr,
                                      const uint8_t txn_id[XSTUN_TXN_ID_SIZE]);

/**
 * @brief Write a MAPPED-ADDRESS attribute.
 */
xErrno xStunAttrWriteMappedAddress(xStunAttrWriter *w, const struct sockaddr *addr);

/**
 * @brief Write a USERNAME attribute.
 *
 * @param w           Attribute writer.
 * @param remote_ufrag Remote ice-ufrag.
 * @param local_ufrag  Local ice-ufrag.
 * @return            xErrno_Ok on success.
 */
xErrno xStunAttrWriteUsername(xStunAttrWriter *w, const char *remote_ufrag,
                              const char *local_ufrag);

/**
 * @brief Write a MESSAGE-INTEGRITY attribute (HMAC-SHA1).
 *
 * Must be the last or second-to-last attribute (before FINGERPRINT).
 * The HMAC is computed over the entire STUN message up to (but not
 * including) this attribute, with the message length field adjusted
 * to include this attribute.
 *
 * @param w        Attribute writer.
 * @param msg_buf  Start of the full STUN message buffer.
 * @param key      HMAC key (ice-pwd as UTF-8 bytes).
 * @param key_len  Length of key.
 * @return         xErrno_Ok on success.
 */
xErrno xStunAttrWriteMessageIntegrity(xStunAttrWriter *w, uint8_t *msg_buf, const uint8_t *key,
                                      size_t key_len);

/**
 * @brief Write a FINGERPRINT attribute (CRC-32).
 *
 * Must be the last attribute. The CRC is computed over the entire
 * STUN message up to (but not including) this attribute, with the
 * message length field adjusted to include this attribute.
 *
 * @param w        Attribute writer.
 * @param msg_buf  Start of the full STUN message buffer.
 * @return         xErrno_Ok on success.
 */
xErrno xStunAttrWriteFingerprint(xStunAttrWriter *w, uint8_t *msg_buf);

/**
 * @brief Write a PRIORITY attribute (32-bit).
 */
xErrno xStunAttrWritePriority(xStunAttrWriter *w, uint32_t priority);

/**
 * @brief Write a USE-CANDIDATE attribute (zero-length).
 */
xErrno xStunAttrWriteUseCandidate(xStunAttrWriter *w);

/**
 * @brief Write an ICE-CONTROLLING attribute (64-bit tie-breaker).
 */
xErrno xStunAttrWriteIceControlling(xStunAttrWriter *w, uint64_t tie_breaker);

/**
 * @brief Write an ICE-CONTROLLED attribute (64-bit tie-breaker).
 */
xErrno xStunAttrWriteIceControlled(xStunAttrWriter *w, uint64_t tie_breaker);

/**
 * @brief Write an ERROR-CODE attribute.
 *
 * @param w       Attribute writer.
 * @param code    Error code (e.g. 401, 420, 487).
 * @param reason  Reason phrase (UTF-8 string, may be NULL).
 * @return        xErrno_Ok on success.
 */
xErrno xStunAttrWriteErrorCode(xStunAttrWriter *w, int code, const char *reason);

/**
 * @brief Write a REALM attribute.
 */
xErrno xStunAttrWriteRealm(xStunAttrWriter *w, const char *realm);

/**
 * @brief Write a NONCE attribute.
 */
xErrno xStunAttrWriteNonce(xStunAttrWriter *w, const char *nonce);

/**
 * @brief Write a LIFETIME attribute (32-bit seconds).
 */
xErrno xStunAttrWriteLifetime(xStunAttrWriter *w, uint32_t lifetime);

/**
 * @brief Write a REQUESTED-TRANSPORT attribute.
 */
xErrno xStunAttrWriteRequestedTransport(xStunAttrWriter *w, uint32_t transport);

/**
 * @brief Write a XOR-PEER-ADDRESS attribute.
 */
xErrno xStunAttrWriteXorPeerAddress(xStunAttrWriter *w, const struct sockaddr *addr,
                                    const uint8_t txn_id[XSTUN_TXN_ID_SIZE]);

/**
 * @brief Write a CHANNEL-NUMBER attribute.
 */
xErrno xStunAttrWriteChannelNumber(xStunAttrWriter *w, uint16_t channel);

/**
 * @brief Write a DATA attribute.
 */
xErrno xStunAttrWriteData(xStunAttrWriter *w, const uint8_t *data, size_t len);

/**
 * @brief Write a SOFTWARE attribute.
 */
xErrno xStunAttrWriteSoftware(xStunAttrWriter *w, const char *software);

/* ───────────────────── Attribute Reader ───────────────────── */

/**
 * @brief Iterator for reading STUN attributes from a decoded message.
 */
XDEF_STRUCT(xStunAttrIter) {
  const uint8_t *data; /**< Attribute payload start. */
  size_t         len;  /**< Total attribute payload length. */
  size_t         pos;  /**< Current read position. */
};

/**
 * @brief A single parsed STUN attribute.
 */
XDEF_STRUCT(xStunAttr) {
  xStunAttrType  type;
  uint16_t       length; /**< Value length (without padding). */
  const uint8_t *value;  /**< Pointer into the original buffer. */
};

/**
 * @brief Initialize an attribute iterator from a decoded STUN message.
 */
void xStunAttrIterInit(xStunAttrIter *iter, const xStunMsg *msg);

/**
 * @brief Advance to the next attribute.
 *
 * @param iter  Iterator.
 * @param attr  Output attribute.
 * @return      true if an attribute was read, false if no more attributes.
 */
bool xStunAttrIterNext(xStunAttrIter *iter, xStunAttr *attr);

/* ───────────────────── Attribute Decoders ───────────────────── */

/**
 * @brief Decode a XOR-MAPPED-ADDRESS attribute value.
 *
 * @param attr    The attribute (type must be XOR-MAPPED-ADDRESS).
 * @param txn_id  Transaction ID for XOR.
 * @param out     Output sockaddr_storage.
 * @return        xErrno_Ok on success.
 */
xErrno xStunAttrDecodeXorMappedAddress(const xStunAttr         *attr,
                                       const uint8_t            txn_id[XSTUN_TXN_ID_SIZE],
                                       struct sockaddr_storage *out);

/**
 * @brief Decode a MAPPED-ADDRESS attribute value.
 */
xErrno xStunAttrDecodeMappedAddress(const xStunAttr *attr, struct sockaddr_storage *out);

/**
 * @brief Decode an ERROR-CODE attribute value.
 *
 * @param attr    The attribute.
 * @param code    Output error code (e.g. 401).
 * @param reason  Output reason phrase pointer (into original buffer).
 * @param reason_len Output reason phrase length.
 * @return        xErrno_Ok on success.
 */
xErrno xStunAttrDecodeErrorCode(const xStunAttr *attr, int *code, const char **reason,
                                size_t *reason_len);

/**
 * @brief Verify MESSAGE-INTEGRITY attribute.
 *
 * @param msg_buf   Full STUN message buffer.
 * @param msg_len   Total message length (header + attrs).
 * @param attr      The MESSAGE-INTEGRITY attribute.
 * @param key       HMAC key.
 * @param key_len   Key length.
 * @return          xErrno_Ok if valid, xErrno_InvalidArg if mismatch.
 */
xErrno xStunAttrVerifyMessageIntegrity(const uint8_t *msg_buf, size_t msg_len,
                                       const xStunAttr *attr, const uint8_t *key, size_t key_len);

/**
 * @brief Verify FINGERPRINT attribute.
 *
 * @param msg_buf   Full STUN message buffer.
 * @param msg_len   Total message length (header + attrs).
 * @param attr      The FINGERPRINT attribute.
 * @return          xErrno_Ok if valid, xErrno_InvalidArg if mismatch.
 */
xErrno xStunAttrVerifyFingerprint(const uint8_t *msg_buf, size_t msg_len, const xStunAttr *attr);

/**
 * @brief Check if an unknown attribute type is comprehension-required.
 *
 * @param type  Attribute type.
 * @return      true if comprehension-required (type < 0x8000).
 */
static inline bool xStunAttrIsComprehensionRequired(xStunAttrType type) {
  return (uint16_t)type < 0x8000;
}

#endif /* XP2P_STUN_ATTR_H */
