/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_channel.h - TURN ChannelData framing (RFC 5766 §11)
 */

#ifndef XP2P_TURN_CHANNEL_H
#define XP2P_TURN_CHANNEL_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <netinet/in.h>

/** ChannelData header size: 2 (channel) + 2 (length). */
#define XTURN_CHANNEL_HEADER_SIZE 4

/**
 * @brief A TURN channel binding.
 */
XDEF_STRUCT(xTurnChannel) {
  uint16_t                number; /**< Channel number (0x4000-0x7FFF). */
  struct sockaddr_storage peer;   /**< Peer address.                   */
  bool                    bound;  /**< Whether the channel is bound.   */
};

/**
 * @brief Check if a byte is a ChannelData first byte.
 *
 * ChannelData messages have first byte in range 0x40-0x7F.
 */
static inline bool xTurnIsChannelData(uint8_t first_byte) {
  return first_byte >= 0x40 && first_byte <= 0x7F;
}

/**
 * @brief Encode a ChannelData message.
 *
 * @param channel  Channel number.
 * @param data     Application data.
 * @param data_len Data length.
 * @param out      Output buffer (must be at least data_len + 4).
 * @param out_cap  Output buffer capacity.
 * @return         Total encoded length, or -1 on error.
 */
int xTurnChannelDataEncode(uint16_t channel, const uint8_t *data,
                           uint16_t data_len, uint8_t *out, size_t out_cap);

/**
 * @brief Decode a ChannelData message.
 *
 * @param buf       Input buffer.
 * @param buf_len   Input buffer length.
 * @param channel   Output channel number.
 * @param data      Output pointer to data within buf.
 * @param data_len  Output data length.
 * @return          xErrno_Ok on success.
 */
xErrno xTurnChannelDataDecode(const uint8_t *buf, size_t buf_len,
                              uint16_t *channel, const uint8_t **data,
                              uint16_t *data_len);

#endif /* XP2P_TURN_CHANNEL_H */
