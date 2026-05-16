/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_channel.c - TURN ChannelData framing (RFC 5766 §11)
 */

#include "turn_channel.h"
#include "ice_private.h"

#include <string.h>

int xTurnChannelDataEncode(uint16_t channel, const uint8_t *data, uint16_t data_len, uint8_t *out,
                           size_t out_cap) {
  size_t total = XTURN_CHANNEL_HEADER_SIZE + XSTUN_ALIGN4(data_len);
  if (total > out_cap) return -1;

  xWriteU16BE(out, channel);
  xWriteU16BE(out + 2, data_len);
  if (data_len > 0 && data) {
    memcpy(out + XTURN_CHANNEL_HEADER_SIZE, data, data_len);
  }
  /* Pad to 4-byte boundary */
  size_t padded = XSTUN_ALIGN4(data_len);
  if (padded > data_len) {
    memset(out + XTURN_CHANNEL_HEADER_SIZE + data_len, 0, padded - data_len);
  }
  return (int)total;
}

xErrno xTurnChannelDataDecode(const uint8_t *buf, size_t buf_len, uint16_t *channel,
                              const uint8_t **data, uint16_t *data_len) {
  if (!buf || !channel || !data || !data_len) return xErrno_InvalidArg;
  if (buf_len < XTURN_CHANNEL_HEADER_SIZE) return xErrno_InvalidArg;

  *channel  = xReadU16BE(buf);
  *data_len = xReadU16BE(buf + 2);

  if (*channel < XTURN_CHANNEL_MIN || *channel > XTURN_CHANNEL_MAX) {
    return xErrno_InvalidArg;
  }

  if (XTURN_CHANNEL_HEADER_SIZE + (size_t)*data_len > buf_len) {
    return xErrno_InvalidArg;
  }

  *data = buf + XTURN_CHANNEL_HEADER_SIZE;
  return xErrno_Ok;
}
