/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sctp_transport.c - SCTP over DTLS transport (usrsctp)
 */

#include "sctp_transport.h"
#include "dtls_transport.h"

#include <usrsctp.h>

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xbase/log.h>

/* ───────────────────── Constants ───────────────────── */

#define XSCTP_DEFAULT_ASSOC_TIMEOUT_MS 10000
#define XSCTP_RECV_BUF_SIZE            65536

/* ───────────────────── Global Init ───────────────────── */

static int g_usrsctp_initialized = 0;

/* ───────────────────── Internal Structure ───────────────────── */

XDEF_STRUCT(xSctpTransport_) {
  xSctpTransportConf conf;
  struct socket     *sock;
  bool               connected;
  xEventTimer        assoc_timer;
};

/* ───────────────────── usrsctp Output Callback ───────────────────── */

/**
 * @brief Called by usrsctp when it has SCTP packets to send.
 *        We encrypt them via DTLS and send through ICE.
 */
static int sctp_output_cb(void *addr, void *buf, size_t len,
                          uint8_t tos __attribute__((unused)),
                          uint8_t set_df __attribute__((unused))) {
  xSctpTransport_ *t = (xSctpTransport_ *)addr;
  if (!t || !t->conf.dtls || !t->sock) {
    XDEBUG("[sctp] output_cb: transport shutting down, drop %zu bytes", len);
    return -1;
  }

  XDEBUG("[sctp] output_cb: sending %zu bytes via DTLS", len);
  xDtlsTransportSend(t->conf.dtls, (const uint8_t *)buf, len);
  return 0;
}

/* ───────────────────── usrsctp Receive Callback ───────────────────── */

static int sctp_recv_cb(struct socket *sock __attribute__((unused)),
                        union sctp_sockstore addr __attribute__((unused)),
                        void *data, size_t datalen,
                        struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
  xSctpTransport_ *t = (xSctpTransport_ *)ulp_info;

  XDEBUG("[sctp] recv_cb: datalen=%zu flags=0x%x", datalen, flags);

  /* Handle SCTP notifications (association changes, stream resets, etc.) */
  if (flags & MSG_NOTIFICATION) {
    if (data && datalen > 0) {
      union sctp_notification *notif = (union sctp_notification *)data;
      if (notif->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
        struct sctp_assoc_change *sac = &notif->sn_assoc_change;
        XDEBUG("[sctp] ASSOC_CHANGE: state=%d", sac->sac_state);
        if (sac->sac_state == SCTP_COMM_UP) {
          t->connected = true;
          /* Cancel association timer */
          if (t->assoc_timer) {
            xEventLoopTimerCancel(t->conf.loop, t->assoc_timer);
            t->assoc_timer = NULL;
          }
          if (t->conf.on_state_change) {
            t->conf.on_state_change((xSctpTransport)t, true, t->conf.ctx);
          }
        } else if (sac->sac_state == SCTP_COMM_LOST ||
                   sac->sac_state == SCTP_SHUTDOWN_COMP) {
          t->connected = false;
          if (t->conf.on_state_change) {
            t->conf.on_state_change((xSctpTransport)t, false, t->conf.ctx);
          }
        }
      } else if (notif->sn_header.sn_type == SCTP_STREAM_RESET_EVENT) {
        struct sctp_stream_reset_event *ssr = &notif->sn_strreset_event;
        uint16_t num_streams =
          (uint16_t)((ssr->strreset_length -
                      sizeof(struct sctp_stream_reset_event)) /
                     sizeof(uint16_t));
        for (uint16_t i = 0; i < num_streams; i++) {
          if (t->conf.on_stream_close) {
            t->conf.on_stream_close((xSctpTransport)t,
                                    ssr->strreset_stream_list[i], t->conf.ctx);
          }
        }
      }
    }
    if (data) free(data);
    return 1;
  }

  if (data == NULL || datalen == 0) {
    if (data) free(data);
    return 1;
  }

  /* Application data */
  uint16_t stream_id = rcv.rcv_sid;
  uint32_t ppid      = ntohl(rcv.rcv_ppid);

  if (t->conf.on_data) {
    t->conf.on_data((xSctpTransport)t, stream_id, ppid,
                    (const uint8_t *)data, datalen, t->conf.ctx);
  }

  free(data);
  return 1;
}

/* ───────────────────── Timer Callbacks ───────────────────── */

static void assoc_timeout_cb(void *arg) {
  xSctpTransport_ *t = (xSctpTransport_ *)arg;
  t->assoc_timer      = NULL;

  if (!t->connected) {
    if (t->conf.on_state_change) {
      t->conf.on_state_change((xSctpTransport)t, false, t->conf.ctx);
    }
  }
}

/* ───────────────────── Public API ───────────────────── */

xSctpTransport xSctpTransportCreate(const xSctpTransportConf *conf) {
  if (!conf || !conf->loop || !conf->dtls) return NULL;

  /* Initialize usrsctp globally (once) */
  if (!g_usrsctp_initialized) {
    usrsctp_init(0, sctp_output_cb, NULL);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_sysctl_set_sctp_nr_outgoing_streams_default(XSCTP_MAX_STREAMS);
    g_usrsctp_initialized = 1;
  }

  xSctpTransport_ *t =
    (xSctpTransport_ *)calloc(1, sizeof(xSctpTransport_));
  if (!t) return NULL;

  t->conf = *conf;
  if (t->conf.local_port == 0) t->conf.local_port = XSCTP_DEFAULT_PORT;
  if (t->conf.remote_port == 0) t->conf.remote_port = XSCTP_DEFAULT_PORT;
  if (t->conf.assoc_timeout_ms == 0) {
    t->conf.assoc_timeout_ms = XSCTP_DEFAULT_ASSOC_TIMEOUT_MS;
  }

  /* Create usrsctp socket */
  t->sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                           sctp_recv_cb, NULL, 0, t);
  if (!t->sock) {
    free(t);
    return NULL;
  }

  /* Register this transport as the "address" for usrsctp output */
  usrsctp_register_address(t);

  /* Enable SCTP stream reset (required for DataChannel close) */
  struct sctp_assoc_value av;
  av.assoc_id    = SCTP_ALL_ASSOC;
  av.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
  usrsctp_setsockopt(t->sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av,
                     sizeof(av));

  /* Enable SCTP notifications */
  struct sctp_event event;
  memset(&event, 0, sizeof(event));
  event.se_assoc_id = SCTP_ALL_ASSOC;
  event.se_on       = 1;

  uint16_t event_types[] = {SCTP_ASSOC_CHANGE, SCTP_STREAM_RESET_EVENT,
                            SCTP_SEND_FAILED_EVENT};
  for (size_t i = 0; i < sizeof(event_types) / sizeof(event_types[0]); i++) {
    event.se_type = event_types[i];
    usrsctp_setsockopt(t->sock, IPPROTO_SCTP, SCTP_EVENT, &event,
                       sizeof(event));
  }

  /* Set non-blocking */
  usrsctp_set_non_blocking(t->sock, 1);

  /* Set nodelay */
  uint32_t nodelay = 1;
  usrsctp_setsockopt(t->sock, IPPROTO_SCTP, SCTP_NODELAY, &nodelay,
                     sizeof(nodelay));

  return (xSctpTransport)t;
}

void xSctpTransportDestroy(xSctpTransport transport) {
  if (!transport) return;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;

  if (t->assoc_timer) {
    xEventLoopTimerCancel(t->conf.loop, t->assoc_timer);
    t->assoc_timer = NULL;
  }

  /* Prevent sctp_output_cb from accessing the DTLS transport
   * while usrsctp's internal timer thread may still fire. */
  t->conf.dtls = NULL;

  if (t->sock) {
    usrsctp_close(t->sock);
    t->sock = NULL;
  }

  usrsctp_deregister_address(t);

  /* Give usrsctp's timer thread a chance to finish any in-flight
   * callback before we free the memory it references. */
  usleep(50000); /* 50 ms */
  free(t);
}

xErrno xSctpTransportStart(xSctpTransport transport) {
  if (!transport) return xErrno_InvalidArg;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;

  /* Bind local address */
  struct sockaddr_conn sconn;
  memset(&sconn, 0, sizeof(sconn));
  sconn.sconn_family = AF_CONN;
  sconn.sconn_port   = htons(t->conf.local_port);
  sconn.sconn_addr   = t;
#ifdef HAVE_SCONN_LEN
  sconn.sconn_len = sizeof(sconn);
#endif

  if (usrsctp_bind(t->sock, (struct sockaddr *)&sconn, sizeof(sconn)) < 0) {
    return xErrno_SysError;
  }

  if (t->conf.is_client) {
    /* Connect to remote */
    struct sockaddr_conn rconn;
    memset(&rconn, 0, sizeof(rconn));
    rconn.sconn_family = AF_CONN;
    rconn.sconn_port   = htons(t->conf.remote_port);
    rconn.sconn_addr   = t;
#ifdef HAVE_SCONN_LEN
    rconn.sconn_len = sizeof(rconn);
#endif

    int ret =
      usrsctp_connect(t->sock, (struct sockaddr *)&rconn, sizeof(rconn));
    if (ret < 0 && errno != EINPROGRESS) {
      return xErrno_SysError;
    }
  } else {
    /* Server side: also use connect (not listen/accept).
     * In WebRTC, SCTP runs over a point-to-point DTLS tunnel,
     * so both sides simply connect to each other. */
    struct sockaddr_conn rconn;
    memset(&rconn, 0, sizeof(rconn));
    rconn.sconn_family = AF_CONN;
    rconn.sconn_port   = htons(t->conf.remote_port);
    rconn.sconn_addr   = t;
#ifdef HAVE_SCONN_LEN
    rconn.sconn_len = sizeof(rconn);
#endif

    int ret =
      usrsctp_connect(t->sock, (struct sockaddr *)&rconn, sizeof(rconn));
    if (ret < 0 && errno != EINPROGRESS) {
      return xErrno_SysError;
    }
  }

  /* Start association timeout */
  t->assoc_timer = xEventLoopTimerAfter(t->conf.loop, assoc_timeout_cb, t,
                                        t->conf.assoc_timeout_ms);

  return xErrno_Ok;
}

xErrno xSctpTransportSend(xSctpTransport transport, uint16_t stream_id,
                           uint32_t ppid, const uint8_t *data, size_t len,
                           bool ordered) {
  if (!transport || !data) return xErrno_InvalidArg;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;

  if (!t->connected) return xErrno_InvalidArg;

  struct sctp_sendv_spa spa;
  memset(&spa, 0, sizeof(spa));
  spa.sendv_flags         = SCTP_SEND_SNDINFO_VALID;
  spa.sendv_sndinfo.snd_sid   = stream_id;
  spa.sendv_sndinfo.snd_ppid  = htonl(ppid);
  spa.sendv_sndinfo.snd_flags = 0;
  if (!ordered) {
    spa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
  }

  ssize_t sent = usrsctp_sendv(t->sock, data, len, NULL, 0, &spa,
                                sizeof(spa), SCTP_SENDV_SPA, 0);
  XDEBUG("[sctp] sendv: len=%zu sent=%zd errno=%d", len, sent, (sent < 0) ? errno : 0);
  return (sent >= 0) ? xErrno_Ok : xErrno_SysError;
}

xErrno xSctpTransportFeedInput(xSctpTransport transport, const uint8_t *data,
                                size_t len) {
  if (!transport || !data || len == 0) return xErrno_InvalidArg;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;

  usrsctp_conninput(t, data, len, 0);
  return xErrno_Ok;
}

xErrno xSctpTransportCloseStream(xSctpTransport transport,
                                  uint16_t       stream_id) {
  if (!transport) return xErrno_InvalidArg;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;

  if (!t->sock) return xErrno_InvalidState;
  if (!t->connected) return xErrno_InvalidState;

  /* sctp_reset_streams uses a flexible array member for srs_stream_list[],
   * so we must allocate enough space for the base struct + 1 stream ID. */
  size_t srs_size = sizeof(struct sctp_reset_streams) + sizeof(uint16_t);
  struct sctp_reset_streams *srs =
    (struct sctp_reset_streams *)calloc(1, srs_size);
  if (!srs) return xErrno_NoMemory;

  srs->srs_flags          = SCTP_STREAM_RESET_OUTGOING;
  srs->srs_number_streams = 1;
  srs->srs_stream_list[0] = stream_id;

  int ret = usrsctp_setsockopt(t->sock, IPPROTO_SCTP, SCTP_RESET_STREAMS,
                                srs, (socklen_t)srs_size);
  free(srs);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

bool xSctpTransportIsConnected(xSctpTransport transport) {
  if (!transport) return false;
  xSctpTransport_ *t = (xSctpTransport_ *)transport;
  return t->connected;
}
