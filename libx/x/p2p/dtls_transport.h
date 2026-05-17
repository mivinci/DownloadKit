/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_transport.h - DTLS transport layer for WebRTC
 *
 * Manages the DTLS handshake lifecycle, fingerprint verification,
 * and encrypted data transport over ICE. Backend-agnostic: delegates
 * to the compiled-in TLS backend (OpenSSL or mbedTLS) via dtls_backend.h.
 */

#ifndef XP2P_DTLS_TRANSPORT_H
#define XP2P_DTLS_TRANSPORT_H

#include "dtls_backend.h"

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

/* ───────────────────── Opaque Handle ───────────────────── */

XDEF_HANDLE(xDtlsTransport);

/* ───────────────────── Callbacks ───────────────────── */

/**
 * @brief Called when the DTLS handshake state changes.
 *
 * @param transport  The DTLS transport handle.
 * @param state      New DTLS state.
 * @param arg        User-provided context.
 */
typedef void (*xDtlsOnStateChange)(xDtlsTransport transport, xDtlsState state, void *arg);

/**
 * @brief Called when decrypted application data is available.
 *
 * @param transport  The DTLS transport handle.
 * @param data       Decrypted data buffer.
 * @param len        Length of data.
 * @param arg        User-provided context.
 */
typedef void (*xDtlsOnData)(xDtlsTransport transport, const uint8_t *data, size_t len, void *arg);

/* ───────────────────── Configuration ───────────────────── */

/**
 * @brief Configuration for creating an xDtlsTransport.
 */
XDEF_STRUCT(xDtlsTransportConf) {
  xEventLoop loop; /**< Event loop for timers.                        */
  xDtlsRole  role; /**< DTLS role (active/passive/actpass).            */

  /** Remote fingerprint from SDP a=fingerprint (raw bytes, or all-zero
   *  if not yet known — will be verified after handshake). */
  uint8_t remote_fingerprint[XDTLS_FINGERPRINT_SIZE];
  bool    verify_fingerprint; /**< Whether to verify remote fingerprint. */

  /** Callback to send encrypted DTLS records through ICE. */
  xDtlsSendFn send_fn;
  void       *send_arg;

  /** State change and data callbacks. */
  xDtlsOnStateChange on_state_change;
  xDtlsOnData        on_data;
  void              *ctx; /**< Forwarded to all callbacks. */

  /** Handshake timeout in milliseconds (0 = default 10000ms). */
  uint32_t handshake_timeout_ms;
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a DTLS transport.
 *
 * Generates a self-signed ECDSA P-256 certificate and computes its
 * SHA-256 fingerprint. Does NOT start the handshake yet.
 *
 * @param conf  Configuration.
 * @return      Transport handle, or NULL on failure.
 */
XCAPI(xDtlsTransport) xDtlsTransportCreate(const xDtlsTransportConf *conf);

/**
 * @brief Destroy a DTLS transport and free all resources.
 *
 * @param transport  Transport handle, or NULL (no-op).
 */
XCAPI(void) xDtlsTransportDestroy(xDtlsTransport transport);

/* ───────────────────── Handshake ───────────────────── */

/**
 * @brief Start the DTLS handshake.
 *
 * For active role: sends ClientHello.
 * For passive role: waits for ClientHello (no-op until data arrives).
 *
 * @param transport  Transport handle.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsTransportStart(xDtlsTransport transport);

/* ───────────────────── Data Path ───────────────────── */

/**
 * @brief Feed received DTLS record data from ICE into the transport.
 *
 * This is called by the ICE agent's demux when a DTLS packet arrives.
 *
 * @param transport  Transport handle.
 * @param data       Raw DTLS record bytes.
 * @param len        Length of data.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsTransportFeedInput(xDtlsTransport transport, const uint8_t *data, size_t len);

/**
 * @brief Send application data through the DTLS encrypted channel.
 *
 * Only valid after handshake is complete (state == Connected).
 *
 * @param transport  Transport handle.
 * @param data       Plaintext data to encrypt and send.
 * @param len        Length of data.
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsTransportSend(xDtlsTransport transport, const uint8_t *data, size_t len);

/* ───────────────────── Queries ───────────────────── */

/**
 * @brief Get the local certificate's SHA-256 fingerprint.
 *
 * @param transport  Transport handle.
 * @param out        Output buffer (XDTLS_FINGERPRINT_SIZE bytes).
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsTransportGetFingerprint(xDtlsTransport transport, uint8_t *out);

/**
 * @brief Get the local fingerprint as a colon-separated hex string.
 *
 * @param transport  Transport handle.
 * @param out        Output buffer (at least XDTLS_FINGERPRINT_STR_SIZE).
 * @return           xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsTransportGetFingerprintStr(xDtlsTransport transport, char *out);

/**
 * @brief Get the current DTLS state.
 *
 * @param transport  Transport handle.
 * @return           Current state.
 */
XCAPI(xDtlsState) xDtlsTransportGetState(xDtlsTransport transport);

/**
 * @brief Get the effective DTLS role (Active or Passive) of this transport.
 *
 * Actpass is resolved to Passive at creation time, so this always returns
 * either xDtlsRole_Active or xDtlsRole_Passive.
 *
 * @param transport  Transport handle.
 * @return           Effective role.
 */
XCAPI(xDtlsRole) xDtlsTransportGetRole(xDtlsTransport transport);

/**
 * @brief Change the DTLS role before the handshake starts.
 *
 * This is useful when the offerer creates the DTLS transport early (to lock
 * in the certificate fingerprint for SDP) but the final role is only known
 * after parsing the remote answer.  The backend context is recreated with
 * the new role.
 *
 * @param transport  Transport handle.
 * @param role       New role (Active, Passive, or Actpass → Passive).
 * @return           xErrno_Ok on success, xErrno_InvalidArg if handshake
 *                   already started.
 */
XCAPI(xErrno) xDtlsTransportSetRole(xDtlsTransport transport, xDtlsRole role);

/* ───────────────────── Utility ───────────────────── */

/**
 * @brief Parse a colon-separated hex fingerprint string into raw bytes.
 *
 * @param str  Fingerprint string (e.g. "AA:BB:CC:...").
 * @param out  Output buffer (XDTLS_FINGERPRINT_SIZE bytes).
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsFingerprintFromStr(const char *str, uint8_t *out);

#endif /* XP2P_DTLS_TRANSPORT_H */
