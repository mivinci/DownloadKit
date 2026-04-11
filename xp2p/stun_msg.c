/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_msg.c - STUN message encoding / decoding (RFC 5389)
 */

#include "stun_msg.h"

#include <string.h>

void xStunMsgInit(xStunMsg *msg, xStunMsgType type,
                  const uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  memset(msg, 0, sizeof(*msg));
  msg->type = type;
  memcpy(msg->txn_id, txn_id, XSTUN_TXN_ID_SIZE);
}

int xStunMsgEncode(const xStunMsg *msg, uint8_t *buf, size_t buf_len) {
  if (!msg || !buf) return -1;

  size_t total = XSTUN_HEADER_SIZE + msg->attrs_len;
  if (buf_len < total) return -1;

  /* Type (2 bytes, big-endian) */
  xWriteU16BE(buf, (uint16_t)msg->type);

  /* Length (2 bytes, big-endian) — payload length, not including header */
  xWriteU16BE(buf + 2, msg->attrs_len);

  /* Magic cookie (4 bytes) */
  xWriteU32BE(buf + 4, XSTUN_MAGIC_COOKIE);

  /* Transaction ID (12 bytes) */
  memcpy(buf + 8, msg->txn_id, XSTUN_TXN_ID_SIZE);

  /* Attribute payload */
  if (msg->attrs_len > 0 && msg->attrs) {
    memcpy(buf + XSTUN_HEADER_SIZE, msg->attrs, msg->attrs_len);
  }

  return (int)total;
}

xErrno xStunMsgDecode(xStunMsg *msg, const uint8_t *buf, size_t buf_len) {
  if (!msg || !buf) return xErrno_InvalidArg;
  if (buf_len < XSTUN_HEADER_SIZE) return xErrno_InvalidArg;

  /* Check first two bits are 0 (STUN messages always have 00 in bits 0-1) */
  if (buf[0] & 0xC0) return xErrno_InvalidArg;

  /* Check magic cookie */
  uint32_t cookie = xReadU32BE(buf + 4);
  if (cookie != XSTUN_MAGIC_COOKIE) return xErrno_InvalidArg;

  msg->type   = (xStunMsgType)xReadU16BE(buf);
  msg->length = xReadU16BE(buf + 2);

  /* Validate that the declared length fits in the buffer */
  if ((size_t)(XSTUN_HEADER_SIZE + msg->length) > buf_len) {
    return xErrno_InvalidArg;
  }

  /* Transaction ID */
  memcpy(msg->txn_id, buf + 8, XSTUN_TXN_ID_SIZE);

  /* Attribute payload */
  msg->attrs_len = msg->length;
  if (msg->attrs_len > 0) {
    msg->attrs = buf + XSTUN_HEADER_SIZE;
  } else {
    msg->attrs = NULL;
  }

  return xErrno_Ok;
}

bool xStunMsgIsStun(const uint8_t *buf, size_t buf_len) {
  if (!buf || buf_len < XSTUN_HEADER_SIZE) return false;

  /* First two bits must be 0 */
  if (buf[0] & 0xC0) return false;

  /* Check magic cookie */
  uint32_t cookie = xReadU32BE(buf + 4);
  return cookie == XSTUN_MAGIC_COOKIE;
}
