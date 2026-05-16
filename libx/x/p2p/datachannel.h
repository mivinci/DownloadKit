/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * datachannel.h - WebRTC DataChannel (DCEP) protocol
 *
 * Implements the DataChannel Establishment Protocol (RFC 8832) on top
 * of SCTP streams. Provides create/accept/send/close operations for
 * WebRTC DataChannels compatible with browser RTCDataChannel.
 */

#ifndef XP2P_DATACHANNEL_H
#define XP2P_DATACHANNEL_H

#include "sctp_transport.h"

#include <x/base/base.h>
#include <x/base/error.h>

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

/** Maximum DataChannel label length. */
#define XDC_MAX_LABEL_LEN 256

/** Maximum DataChannel protocol length. */
#define XDC_MAX_PROTOCOL_LEN 256

/** Maximum number of DataChannels per connection. */
#define XDC_MAX_CHANNELS 64

/* ───────────────────── DCEP Message Types (RFC 8832) ───────────────────── */

#define XDCEP_DATA_CHANNEL_OPEN 0x03
#define XDCEP_DATA_CHANNEL_ACK  0x02

/* ───────────────────── DCEP Channel Types (RFC 8832) ───────────────────── */

#define XDCEP_CHANNEL_RELIABLE                0x00
#define XDCEP_CHANNEL_RELIABLE_UNORDERED      0x80
#define XDCEP_CHANNEL_PARTIAL_RTXS            0x01
#define XDCEP_CHANNEL_PARTIAL_RTXS_UNORDERED  0x81
#define XDCEP_CHANNEL_PARTIAL_TIME            0x02
#define XDCEP_CHANNEL_PARTIAL_TIME_UNORDERED  0x82

/* ───────────────────── DataChannel State ───────────────────── */

XDEF_ENUM(xDataChannelState){
  xDataChannelState_Connecting = 0, /**< OPEN sent, waiting for ACK.  */
  xDataChannelState_Open       = 1, /**< Channel is open for data.    */
  xDataChannelState_Closing    = 2, /**< Close initiated.             */
  xDataChannelState_Closed     = 3, /**< Channel is closed.           */
};

/* ───────────────────── DataChannel Message Type ───────────────────── */

XDEF_ENUM(xDataChannelMsgType){
  xDataChannelMsgType_String = 0, /**< UTF-8 string message.  */
  xDataChannelMsgType_Binary = 1, /**< Binary message.        */
};

/* ───────────────────── Opaque Handle ───────────────────── */

XDEF_HANDLE(xDataChannel);

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when a DataChannel is opened (either locally created
 *        and ACK'd, or remotely opened).
 */
typedef void (*xDataChannelOnOpen)(xDataChannel channel, void *arg);

/**
 * @brief Called when a DataChannel receives a message.
 */
typedef void (*xDataChannelOnMessage)(xDataChannel channel,
                                      xDataChannelMsgType type,
                                      const uint8_t *data, size_t len,
                                      void *arg);

/**
 * @brief Called when a DataChannel is closed.
 */
typedef void (*xDataChannelOnClose)(xDataChannel channel, void *arg);

/**
 * @brief Called when a DataChannel encounters an error.
 */
typedef void (*xDataChannelOnError)(xDataChannel channel, xErrno err,
                                    void *arg);

/**
 * @brief Called when the DataChannel's buffered amount drops to or
 *        below the configured low-water threshold.
 *
 * Used for backpressure: pause sending when buffered amount is high,
 * resume when this callback fires.
 */
typedef void (*xDataChannelOnBufferedAmountLow)(xDataChannel channel,
                                                void *arg);

/* ───────────────────── DataChannel Manager ───────────────────── */

/**
 * @brief Opaque DataChannel manager that owns all channels on a
 *        single SCTP association.
 */
XDEF_HANDLE(xDataChannelMgr);

/**
 * @brief Called when a remote peer opens a new DataChannel.
 */
typedef void (*xDataChannelOnRemoteOpen)(xDataChannelMgr mgr,
                                         xDataChannel    channel, void *arg);

/* ───────────────────── Manager Configuration ───────────────────── */

XDEF_STRUCT(xDataChannelMgrConf) {
  xSctpTransport sctp; /**< SCTP transport to use.                     */

  /** Called when remote peer opens a DataChannel. */
  xDataChannelOnRemoteOpen on_remote_open;

  /** Default callbacks for remotely-opened channels. */
  xDataChannelOnOpen    on_open;
  xDataChannelOnMessage on_message;
  xDataChannelOnClose   on_close;
  xDataChannelOnError   on_error;
  void                 *ctx;
};

/* ───────────────────── DataChannel Configuration ───────────────────── */

XDEF_STRUCT(xDataChannelConf) {
  char     label[XDC_MAX_LABEL_LEN];       /**< Channel label.          */
  char     protocol[XDC_MAX_PROTOCOL_LEN]; /**< Sub-protocol.           */
  bool     ordered;          /**< Ordered delivery (default true).      */
  uint16_t max_retransmits;  /**< Max retransmits (0 = reliable).       */
  uint16_t max_packet_life_time; /**< Max lifetime ms (0 = reliable).   */

  /** Per-channel callbacks (override manager defaults if non-NULL). */
  xDataChannelOnOpen              on_open;
  xDataChannelOnMessage           on_message;
  xDataChannelOnClose             on_close;
  xDataChannelOnError             on_error;
  xDataChannelOnBufferedAmountLow on_buffered_amount_low;
  void                           *ctx;
};

/* ───────────────────── Manager API ───────────────────── */

/**
 * @brief Create a DataChannel manager.
 *
 * @param conf  Configuration.
 * @return      Manager handle, or NULL on failure.
 */
XCAPI(xDataChannelMgr) xDataChannelMgrCreate(const xDataChannelMgrConf *conf);

/**
 * @brief Destroy the DataChannel manager and all channels.
 */
XCAPI(void) xDataChannelMgrDestroy(xDataChannelMgr mgr);

/**
 * @brief Handle incoming SCTP data for DataChannel processing.
 *
 * Should be called from the SCTP transport's on_data callback.
 * Routes DCEP control messages and application data to the
 * appropriate DataChannel.
 *
 * @param mgr        Manager handle.
 * @param stream_id  SCTP stream ID.
 * @param ppid       SCTP PPID.
 * @param data       Received data.
 * @param len        Length of data.
 */
XCAPI(void) xDataChannelMgrOnData(xDataChannelMgr mgr, uint16_t stream_id,
                                   uint32_t ppid, const uint8_t *data,
                                   size_t len);

/**
 * @brief Handle SCTP stream close notification.
 */
XCAPI(void) xDataChannelMgrOnStreamClose(xDataChannelMgr mgr,
                                          uint16_t        stream_id);

/* ───────────────────── Channel API ───────────────────── */

/**
 * @brief Create and open a new DataChannel.
 *
 * Sends a DATA_CHANNEL_OPEN message on the next available stream.
 *
 * @param mgr   Manager handle.
 * @param conf  Channel configuration.
 * @return      Channel handle, or NULL on failure.
 */
XCAPI(xDataChannel) xDataChannelCreate(xDataChannelMgr        mgr,
                                        const xDataChannelConf *conf);

/**
 * @brief Send a string message on a DataChannel.
 */
XCAPI(xErrno) xDataChannelSendString(xDataChannel channel, const char *str,
                                      size_t len);

/**
 * @brief Send a binary message on a DataChannel.
 */
XCAPI(xErrno) xDataChannelSendBinary(xDataChannel channel, const uint8_t *data,
                                      size_t len);

/**
 * @brief Close a DataChannel.
 */
XCAPI(void) xDataChannelClose(xDataChannel channel);

/**
 * @brief Get the label of a DataChannel.
 */
XCAPI(const char *) xDataChannelGetLabel(xDataChannel channel);

/**
 * @brief Get the current state of a DataChannel.
 */
XCAPI(xDataChannelState) xDataChannelGetState(xDataChannel channel);

/**
 * @brief Get the SCTP stream ID of a DataChannel.
 */
XCAPI(uint16_t) xDataChannelGetStreamId(xDataChannel channel);

/**
 * @brief Get the amount of data buffered for sending on this channel.
 *
 * @param channel  DataChannel handle.
 * @return         Bytes currently buffered.
 */
XCAPI(size_t) xDataChannelGetBufferedAmount(xDataChannel channel);

/**
 * @brief Set the low-water threshold for the buffered amount.
 *
 * When the buffered amount drops to or below this value, the
 * on_buffered_amount_low callback fires.
 *
 * @param channel    DataChannel handle.
 * @param threshold  Threshold in bytes (default 0).
 */
XCAPI(void) xDataChannelSetBufferedAmountLowThreshold(xDataChannel channel,
                                                       size_t threshold);

/**
 * @brief Notify the DataChannel manager that the SCTP send buffer
 *        has been drained.
 *
 * Should be called from the SCTP transport's on_buffered_amount_low
 * callback. Resets per-channel buffered amounts and fires any
 * pending on_buffered_amount_low callbacks.
 *
 * @param mgr  Manager handle.
 */
XCAPI(void) xDataChannelMgrOnBufferedAmountLow(xDataChannelMgr mgr);

#endif /* XP2P_DATACHANNEL_H */
