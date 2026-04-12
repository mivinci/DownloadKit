/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * datachannel.c - WebRTC DataChannel (DCEP) implementation
 */

#include "datachannel.h"
#include "sctp_transport.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal Structures ───────────────────── */

XDEF_STRUCT(xDataChannel_) {
  uint16_t          stream_id;
  xDataChannelState state;
  xDataChannelConf  conf;
  xDataChannelMgr   mgr; /* Back-pointer to owning manager */
  size_t            buffered_amount;               /**< Tracked send-buffer usage. */
  size_t            buffered_amount_low_threshold;  /**< Low-water mark.            */
};

XDEF_STRUCT(xDataChannelMgr_) {
  xDataChannelMgrConf conf;
  xDataChannel_      *channels[XDC_MAX_CHANNELS];
  int                 channel_count;
  uint16_t            next_stream_id; /* Next stream ID for locally-created channels */
};

/* ───────────────────── DCEP Encoding ───────────────────── */

/**
 * @brief Encode a DATA_CHANNEL_OPEN message (RFC 8832 §5.1).
 *
 * Format:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  Message Type |  Channel Type |          Priority             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                    Reliability Parameter                      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         Label Length          |       Protocol Length         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  |                         Label (UTF-8)                         |
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  |                        Protocol (UTF-8)                       |
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
static int dcep_encode_open(const xDataChannelConf *conf, uint8_t *buf,
                            size_t cap) {
  size_t label_len    = strlen(conf->label);
  size_t protocol_len = strlen(conf->protocol);
  size_t total        = 12 + label_len + protocol_len;

  if (total > cap) return -1;

  /* Message Type */
  buf[0] = XDCEP_DATA_CHANNEL_OPEN;

  /* Channel Type */
  uint8_t channel_type;
  if (conf->max_retransmits > 0) {
    channel_type = conf->ordered ? XDCEP_CHANNEL_PARTIAL_RTXS
                                 : XDCEP_CHANNEL_PARTIAL_RTXS_UNORDERED;
  } else if (conf->max_packet_life_time > 0) {
    channel_type = conf->ordered ? XDCEP_CHANNEL_PARTIAL_TIME
                                 : XDCEP_CHANNEL_PARTIAL_TIME_UNORDERED;
  } else {
    channel_type = conf->ordered ? XDCEP_CHANNEL_RELIABLE
                                 : XDCEP_CHANNEL_RELIABLE_UNORDERED;
  }
  buf[1] = channel_type;

  /* Priority (default 0) */
  buf[2] = 0;
  buf[3] = 0;

  /* Reliability Parameter */
  uint32_t reliability = 0;
  if (conf->max_retransmits > 0) {
    reliability = conf->max_retransmits;
  } else if (conf->max_packet_life_time > 0) {
    reliability = conf->max_packet_life_time;
  }
  buf[4] = (uint8_t)(reliability >> 24);
  buf[5] = (uint8_t)(reliability >> 16);
  buf[6] = (uint8_t)(reliability >> 8);
  buf[7] = (uint8_t)(reliability);

  /* Label Length */
  buf[8] = (uint8_t)(label_len >> 8);
  buf[9] = (uint8_t)(label_len);

  /* Protocol Length */
  buf[10] = (uint8_t)(protocol_len >> 8);
  buf[11] = (uint8_t)(protocol_len);

  /* Label */
  if (label_len > 0) {
    memcpy(buf + 12, conf->label, label_len);
  }

  /* Protocol */
  if (protocol_len > 0) {
    memcpy(buf + 12 + label_len, conf->protocol, protocol_len);
  }

  return (int)total;
}

/**
 * @brief Decode a DATA_CHANNEL_OPEN message.
 */
static xErrno dcep_decode_open(const uint8_t *data, size_t len,
                               xDataChannelConf *out) {
  if (len < 12) return xErrno_InvalidArg;
  if (data[0] != XDCEP_DATA_CHANNEL_OPEN) return xErrno_InvalidArg;

  memset(out, 0, sizeof(*out));

  uint8_t channel_type = data[1];

  /* Decode ordered flag */
  out->ordered = !(channel_type & 0x80);

  /* Decode reliability */
  uint32_t reliability =
    ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
    ((uint32_t)data[6] << 8) | (uint32_t)data[7];

  uint8_t base_type = channel_type & 0x7F;
  if (base_type == 0x01) {
    out->max_retransmits = (uint16_t)reliability;
  } else if (base_type == 0x02) {
    out->max_packet_life_time = (uint16_t)reliability;
  }

  /* Label */
  uint16_t label_len = (uint16_t)((data[8] << 8) | data[9]);
  uint16_t proto_len = (uint16_t)((data[10] << 8) | data[11]);

  if ((size_t)12 + label_len + proto_len > len) return xErrno_InvalidArg;

  if (label_len > 0 && label_len < XDC_MAX_LABEL_LEN) {
    memcpy(out->label, data + 12, label_len);
    out->label[label_len] = '\0';
  }

  if (proto_len > 0 && proto_len < XDC_MAX_PROTOCOL_LEN) {
    memcpy(out->protocol, data + 12 + label_len, proto_len);
    out->protocol[proto_len] = '\0';
  }

  return xErrno_Ok;
}

/**
 * @brief Encode a DATA_CHANNEL_ACK message (just 1 byte).
 */
static int dcep_encode_ack(uint8_t *buf, size_t cap) {
  if (cap < 1) return -1;
  buf[0] = XDCEP_DATA_CHANNEL_ACK;
  return 1;
}

/* ───────────────────── Manager API ───────────────────── */

xDataChannelMgr xDataChannelMgrCreate(const xDataChannelMgrConf *conf) {
  if (!conf || !conf->sctp) return NULL;

  xDataChannelMgr_ *mgr =
    (xDataChannelMgr_ *)calloc(1, sizeof(xDataChannelMgr_));
  if (!mgr) return NULL;

  mgr->conf = *conf;
  /* Local channels use even stream IDs (DTLS client) or odd (DTLS server).
   * For simplicity, start at 0 and increment by 2. */
  mgr->next_stream_id = 0;

  return (xDataChannelMgr)mgr;
}

void xDataChannelMgrDestroy(xDataChannelMgr mgr_handle) {
  if (!mgr_handle) return;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)mgr_handle;

  for (int i = 0; i < mgr->channel_count; i++) {
    if (mgr->channels[i]) {
      free(mgr->channels[i]);
    }
  }
  free(mgr);
}

static xDataChannel_ *find_channel(xDataChannelMgr_ *mgr,
                                    uint16_t          stream_id) {
  for (int i = 0; i < mgr->channel_count; i++) {
    if (mgr->channels[i] && mgr->channels[i]->stream_id == stream_id) {
      return mgr->channels[i];
    }
  }
  return NULL;
}

static xDataChannel_ *add_channel(xDataChannelMgr_ *mgr, uint16_t stream_id,
                                   const xDataChannelConf *conf) {
  if (mgr->channel_count >= XDC_MAX_CHANNELS) return NULL;

  xDataChannel_ *ch = (xDataChannel_ *)calloc(1, sizeof(xDataChannel_));
  if (!ch) return NULL;

  ch->stream_id = stream_id;
  ch->state     = xDataChannelState_Connecting;
  ch->conf      = *conf;
  ch->mgr       = (xDataChannelMgr)mgr;

  mgr->channels[mgr->channel_count++] = ch;
  return ch;
}

void xDataChannelMgrOnData(xDataChannelMgr mgr_handle, uint16_t stream_id,
                            uint32_t ppid, const uint8_t *data, size_t len) {
  if (!mgr_handle || !data) return;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)mgr_handle;

  if (ppid == XSCTP_PPID_DCEP) {
    /* DCEP control message */
    if (len < 1) return;

    if (data[0] == XDCEP_DATA_CHANNEL_OPEN) {
      /* Remote peer is opening a DataChannel */
      xDataChannelConf conf;
      if (dcep_decode_open(data, len, &conf) != xErrno_Ok) return;

      /* Use manager's default callbacks */
      conf.on_open    = mgr->conf.on_open;
      conf.on_message = mgr->conf.on_message;
      conf.on_close   = mgr->conf.on_close;
      conf.on_error   = mgr->conf.on_error;
      conf.ctx        = mgr->conf.ctx;

      xDataChannel_ *ch = add_channel(mgr, stream_id, &conf);
      if (!ch) return;

      /* Send ACK */
      uint8_t ack_buf[1];
      int     ack_len = dcep_encode_ack(ack_buf, sizeof(ack_buf));
      if (ack_len > 0) {
        xSctpTransportSend(mgr->conf.sctp, stream_id, XSCTP_PPID_DCEP,
                           ack_buf, (size_t)ack_len, true);
      }

      ch->state = xDataChannelState_Open;

      /* Notify application */
      if (mgr->conf.on_remote_open) {
        mgr->conf.on_remote_open(mgr_handle, (xDataChannel)ch, mgr->conf.ctx);
      }
      if (ch->conf.on_open) {
        ch->conf.on_open((xDataChannel)ch, ch->conf.ctx);
      }

    } else if (data[0] == XDCEP_DATA_CHANNEL_ACK) {
      /* ACK for a channel we opened */
      xDataChannel_ *ch = find_channel(mgr, stream_id);
      if (ch && ch->state == xDataChannelState_Connecting) {
        ch->state = xDataChannelState_Open;
        if (ch->conf.on_open) {
          ch->conf.on_open((xDataChannel)ch, ch->conf.ctx);
        }
      }
    }
    return;
  }

  /* Application data */
  xDataChannel_ *ch = find_channel(mgr, stream_id);
  if (!ch || ch->state != xDataChannelState_Open) return;

  xDataChannelMsgType msg_type;
  if (ppid == XSCTP_PPID_STRING || ppid == XSCTP_PPID_STRING_EMPTY) {
    msg_type = xDataChannelMsgType_String;
  } else {
    msg_type = xDataChannelMsgType_Binary;
  }

  if (ch->conf.on_message) {
    ch->conf.on_message((xDataChannel)ch, msg_type, data, len, ch->conf.ctx);
  }
}

void xDataChannelMgrOnStreamClose(xDataChannelMgr mgr_handle,
                                   uint16_t        stream_id) {
  if (!mgr_handle) return;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)mgr_handle;

  xDataChannel_ *ch = find_channel(mgr, stream_id);
  if (!ch) return;

  ch->state = xDataChannelState_Closed;
  if (ch->conf.on_close) {
    ch->conf.on_close((xDataChannel)ch, ch->conf.ctx);
  }
}

/* ───────────────────── Channel API ───────────────────── */

xDataChannel xDataChannelCreate(xDataChannelMgr        mgr_handle,
                                 const xDataChannelConf *conf) {
  if (!mgr_handle || !conf) return NULL;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)mgr_handle;

  uint16_t stream_id = mgr->next_stream_id;
  mgr->next_stream_id += 2; /* Even IDs for local channels */

  xDataChannel_ *ch = add_channel(mgr, stream_id, conf);
  if (!ch) return NULL;

  /* Send DATA_CHANNEL_OPEN */
  uint8_t open_buf[512];
  int     open_len = dcep_encode_open(conf, open_buf, sizeof(open_buf));
  if (open_len < 0) {
    /* Remove channel on failure */
    mgr->channel_count--;
    free(ch);
    return NULL;
  }

  xErrno err = xSctpTransportSend(mgr->conf.sctp, stream_id, XSCTP_PPID_DCEP,
                                  open_buf, (size_t)open_len, true);
  if (err != xErrno_Ok) {
    mgr->channel_count--;
    free(ch);
    return NULL;
  }

  return (xDataChannel)ch;
}

xErrno xDataChannelSendString(xDataChannel channel, const char *str,
                               size_t len) {
  if (!channel || !str) return xErrno_InvalidArg;
  xDataChannel_    *ch  = (xDataChannel_ *)channel;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)ch->mgr;

  if (ch->state != xDataChannelState_Open) return xErrno_InvalidArg;

  uint32_t ppid = (len == 0) ? XSCTP_PPID_STRING_EMPTY : XSCTP_PPID_STRING;
  return xSctpTransportSend(mgr->conf.sctp, ch->stream_id, ppid,
                            (const uint8_t *)str, len, ch->conf.ordered);
}

xErrno xDataChannelSendBinary(xDataChannel channel, const uint8_t *data,
                               size_t len) {
  if (!channel || !data) return xErrno_InvalidArg;
  xDataChannel_    *ch  = (xDataChannel_ *)channel;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)ch->mgr;

  if (ch->state != xDataChannelState_Open) return xErrno_InvalidArg;

  uint32_t ppid = (len == 0) ? XSCTP_PPID_BINARY_EMPTY : XSCTP_PPID_BINARY;
  xErrno err = xSctpTransportSend(mgr->conf.sctp, ch->stream_id, ppid, data,
                                  len, ch->conf.ordered);
  if (err == xErrno_Ok) {
    ch->buffered_amount += len;
  }
  return err;
}

void xDataChannelClose(xDataChannel channel) {
  if (!channel) return;
  xDataChannel_    *ch  = (xDataChannel_ *)channel;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)ch->mgr;

  if (ch->state == xDataChannelState_Closed) return;

  ch->state = xDataChannelState_Closing;
  xSctpTransportCloseStream(mgr->conf.sctp, ch->stream_id);
}

const char *xDataChannelGetLabel(xDataChannel channel) {
  if (!channel) return "";
  xDataChannel_ *ch = (xDataChannel_ *)channel;
  return ch->conf.label;
}

xDataChannelState xDataChannelGetState(xDataChannel channel) {
  if (!channel) return xDataChannelState_Closed;
  xDataChannel_ *ch = (xDataChannel_ *)channel;
  return ch->state;
}

uint16_t xDataChannelGetStreamId(xDataChannel channel) {
  if (!channel) return 0;
  xDataChannel_ *ch = (xDataChannel_ *)channel;
  return ch->stream_id;
}

size_t xDataChannelGetBufferedAmount(xDataChannel channel) {
  if (!channel) return 0;
  xDataChannel_ *ch = (xDataChannel_ *)channel;
  return ch->buffered_amount;
}

void xDataChannelSetBufferedAmountLowThreshold(xDataChannel channel,
                                                size_t threshold) {
  if (!channel) return;
  xDataChannel_ *ch = (xDataChannel_ *)channel;
  ch->buffered_amount_low_threshold = threshold;
}

void xDataChannelMgrOnBufferedAmountLow(xDataChannelMgr mgr_handle) {
  if (!mgr_handle) return;
  xDataChannelMgr_ *mgr = (xDataChannelMgr_ *)mgr_handle;

  for (int i = 0; i < mgr->channel_count; i++) {
    xDataChannel_ *ch = mgr->channels[i];
    if (!ch || ch->state != xDataChannelState_Open) continue;

    /* Reset buffered amount since SCTP has drained. */
    ch->buffered_amount = 0;

    if (ch->conf.on_buffered_amount_low) {
      ch->conf.on_buffered_amount_low((xDataChannel)ch, ch->conf.ctx);
    }
  }
}
