/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sctp_transport.h - SCTP over DTLS transport for WebRTC DataChannel
 *
 * Wraps usrsctp to provide a user-space SCTP association that runs
 * on top of the DTLS encrypted channel. Handles SCTP init, data
 * send/recv, and stream management.
 */

#ifndef XP2P_SCTP_TRANSPORT_H
#define XP2P_SCTP_TRANSPORT_H

#include "dtls_transport.h"

#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>

#include <stdbool.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

/** Default SCTP port for WebRTC DataChannel. */
#define XSCTP_DEFAULT_PORT 5000

/** Maximum number of SCTP streams. */
#define XSCTP_MAX_STREAMS 256

/* ───────────────────── SCTP PPID Values (RFC 8831) ───────────────────── */

/** PPID for WebRTC DataChannel Control (DCEP). */
#define XSCTP_PPID_DCEP   50

/** PPID for WebRTC String (UTF-8). */
#define XSCTP_PPID_STRING 51

/** PPID for WebRTC Binary. */
#define XSCTP_PPID_BINARY 53

/** PPID for WebRTC String Empty. */
#define XSCTP_PPID_STRING_EMPTY 56

/** PPID for WebRTC Binary Empty. */
#define XSCTP_PPID_BINARY_EMPTY 57

/* ───────────────────── Opaque Handle ───────────────────── */

XDEF_HANDLE(xSctpTransport);

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when the SCTP association state changes.
 */
typedef void (*xSctpOnStateChange)(xSctpTransport transport, bool connected,
                                   void *arg);

/**
 * @brief Called when data is received on an SCTP stream.
 *
 * @param transport  SCTP transport handle.
 * @param stream_id  SCTP stream identifier.
 * @param ppid       Payload Protocol Identifier.
 * @param data       Received data.
 * @param len        Length of data.
 * @param arg        User-provided context.
 */
typedef void (*xSctpOnData)(xSctpTransport transport, uint16_t stream_id,
                            uint32_t ppid, const uint8_t *data, size_t len,
                            void *arg);

/**
 * @brief Called when a new SCTP stream is opened by the remote peer.
 */
typedef void (*xSctpOnStreamOpen)(xSctpTransport transport, uint16_t stream_id,
                                  void *arg);

/**
 * @brief Called when an SCTP stream is closed.
 */
typedef void (*xSctpOnStreamClose)(xSctpTransport transport,
                                   uint16_t stream_id, void *arg);

/* ───────────────────── Configuration ───────────────────── */

XDEF_STRUCT(xSctpTransportConf) {
  xEventLoop     loop;      /**< Event loop for timers.                  */
  xDtlsTransport dtls;      /**< DTLS transport for encrypted I/O.      */
  bool           is_client; /**< true = initiate SCTP, false = accept.   */
  uint16_t       local_port;  /**< Local SCTP port (0 = default 5000).  */
  uint16_t       remote_port; /**< Remote SCTP port (0 = default 5000). */

  /** Callbacks. */
  xSctpOnStateChange on_state_change;
  xSctpOnData        on_data;
  xSctpOnStreamOpen  on_stream_open;
  xSctpOnStreamClose on_stream_close;
  void              *ctx;

  /** Association timeout in milliseconds (0 = default 10000ms). */
  uint32_t assoc_timeout_ms;
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create an SCTP transport over DTLS.
 *
 * Initializes usrsctp and creates a socket. Does NOT start the
 * association yet — call xSctpTransportStart after DTLS is connected.
 *
 * @param conf  Configuration.
 * @return      Transport handle, or NULL on failure.
 */
XCAPI(xSctpTransport) xSctpTransportCreate(const xSctpTransportConf *conf);

/**
 * @brief Destroy the SCTP transport.
 */
XCAPI(void) xSctpTransportDestroy(xSctpTransport transport);

/**
 * @brief Start the SCTP association.
 *
 * For client: sends INIT. For server: listens for INIT.
 * Should be called after DTLS handshake completes.
 *
 * @param transport  Transport handle.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xSctpTransportStart(xSctpTransport transport);

/* ───────────────────── Data Path ───────────────────── */

/**
 * @brief Send data on an SCTP stream.
 *
 * @param transport  Transport handle.
 * @param stream_id  SCTP stream identifier.
 * @param ppid       Payload Protocol Identifier.
 * @param data       Data to send.
 * @param len        Length of data.
 * @param ordered    Whether to send ordered.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xSctpTransportSend(xSctpTransport transport, uint16_t stream_id,
                                  uint32_t ppid, const uint8_t *data,
                                  size_t len, bool ordered);

/**
 * @brief Feed decrypted SCTP data from DTLS into usrsctp.
 *
 * Called by the DTLS transport's on_data callback.
 *
 * @param transport  Transport handle.
 * @param data       Decrypted SCTP packet data.
 * @param len        Length of data.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xSctpTransportFeedInput(xSctpTransport transport,
                                       const uint8_t *data, size_t len);

/**
 * @brief Close an SCTP stream (reset).
 *
 * @param transport  Transport handle.
 * @param stream_id  Stream to close.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xSctpTransportCloseStream(xSctpTransport transport,
                                         uint16_t       stream_id);

/**
 * @brief Check if the SCTP association is established.
 */
XCAPI(bool) xSctpTransportIsConnected(xSctpTransport transport);

#endif /* XP2P_SCTP_TRANSPORT_H */
