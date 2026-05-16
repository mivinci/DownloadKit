/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_msg.h - STUN message encoding / decoding (RFC 5389)
 */

#ifndef XP2P_STUN_MSG_H
#define XP2P_STUN_MSG_H

#include "ice_private.h"

#include <stdint.h>

/**
 * @brief STUN message structure.
 *
 * Represents a parsed STUN message header plus a pointer to the
 * raw attribute payload. The payload is NOT owned by this struct;
 * it points into the original decode buffer or a user-supplied
 * encode buffer.
 */
XDEF_STRUCT(xStunMsg) {
  xStunMsgType   type;                      /**< Message type.              */
  uint16_t       length;                    /**< Payload length (bytes).    */
  uint8_t        txn_id[XSTUN_TXN_ID_SIZE]; /**< Transaction ID.       */
  const uint8_t *attrs;                     /**< Pointer to attribute data. */
  uint16_t       attrs_len;                 /**< Length of attribute data.  */
};

/**
 * @brief Initialize a STUN message with the given type and transaction ID.
 *
 * @param msg     Message to initialize.
 * @param type    Message type.
 * @param txn_id  12-byte transaction ID (copied in).
 */
void xStunMsgInit(xStunMsg *msg, xStunMsgType type, const uint8_t txn_id[XSTUN_TXN_ID_SIZE]);

/**
 * @brief Encode a STUN message into a byte buffer.
 *
 * Writes the 20-byte STUN header followed by the attribute payload
 * (msg->attrs, msg->attrs_len) into @p buf.
 *
 * @param msg      Message to encode.
 * @param buf      Output buffer.
 * @param buf_len  Size of output buffer in bytes.
 * @return         Total encoded length on success, or -1 if buffer
 *                 is too small.
 */
int xStunMsgEncode(const xStunMsg *msg, uint8_t *buf, size_t buf_len);

/**
 * @brief Decode a STUN message from a byte buffer.
 *
 * Parses the 20-byte header and sets msg->attrs to point into @p buf
 * at the attribute payload offset.
 *
 * @param msg      Output message structure.
 * @param buf      Input buffer containing a STUN message.
 * @param buf_len  Length of input buffer in bytes.
 * @return         xErrno_Ok on success, or an error code.
 */
xErrno xStunMsgDecode(xStunMsg *msg, const uint8_t *buf, size_t buf_len);

/**
 * @brief Check if a byte buffer looks like a STUN message.
 *
 * Checks that the first two bits are 0 and the magic cookie is present.
 * Does NOT validate the full message.
 *
 * @param buf      Input buffer.
 * @param buf_len  Length of input buffer in bytes.
 * @return         true if the buffer appears to be a STUN message.
 */
bool xStunMsgIsStun(const uint8_t *buf, size_t buf_len);

/**
 * @brief Check if a STUN message type is a request.
 */
static inline bool xStunMsgIsRequest(xStunMsgType type) {
  return XSTUN_MSG_CLASS(type) == XSTUN_CLASS_REQUEST;
}

/**
 * @brief Check if a STUN message type is an indication.
 */
static inline bool xStunMsgIsIndication(xStunMsgType type) {
  return XSTUN_MSG_CLASS(type) == XSTUN_CLASS_INDICATION;
}

/**
 * @brief Check if a STUN message type is a success response.
 */
static inline bool xStunMsgIsSuccessResponse(xStunMsgType type) {
  return XSTUN_MSG_CLASS(type) == XSTUN_CLASS_SUCCESS_RESP;
}

/**
 * @brief Check if a STUN message type is an error response.
 */
static inline bool xStunMsgIsErrorResponse(xStunMsgType type) {
  return XSTUN_MSG_CLASS(type) == XSTUN_CLASS_ERROR_RESP;
}

#endif /* XP2P_STUN_MSG_H */
