/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_backend.h - TLS backend abstraction for DTLS transport
 *
 * Defines a function-pointer table that each TLS backend (OpenSSL,
 * mbedTLS) must implement. The xDtlsTransport calls through this
 * vtable so the core logic is backend-agnostic.
 */

#ifndef XP2P_DTLS_BACKEND_H
#define XP2P_DTLS_BACKEND_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ───────────────────── Constants ───────────────────── */

/** SHA-256 fingerprint length in bytes. */
#define XDTLS_FINGERPRINT_SIZE 32

/** SHA-256 fingerprint as hex string: "AA:BB:CC:..." (32*3-1 + NUL). */
#define XDTLS_FINGERPRINT_STR_SIZE 96

/** Maximum certificate DER size. */
#define XDTLS_MAX_CERT_SIZE 2048

/* ───────────────────── DTLS Setup Role ───────────────────── */

/**
 * @brief DTLS setup role, derived from SDP a=setup attribute.
 */
XDEF_ENUM(xDtlsRole){
  xDtlsRole_Active  = 0, /**< Client: initiates DTLS handshake.  */
  xDtlsRole_Passive = 1, /**< Server: waits for DTLS handshake.  */
  xDtlsRole_Actpass = 2, /**< Can be either (offer default).     */
};

/* ───────────────────── DTLS Handshake State ───────────────────── */

XDEF_ENUM(xDtlsState){
  xDtlsState_New         = 0, /**< Not started.                       */
  xDtlsState_Connecting  = 1, /**< Handshake in progress.             */
  xDtlsState_Connected   = 2, /**< Handshake complete, data flowing.  */
  xDtlsState_Failed      = 3, /**< Handshake failed or timed out.     */
  xDtlsState_Closed      = 4, /**< Shut down.                         */
};

/* ───────────────────── Backend Opaque Context ───────────────────── */

/** Opaque backend-specific context (e.g. SSL_CTX + SSL for OpenSSL). */
typedef struct xDtlsBackendCtx xDtlsBackendCtx;

/* ───────────────────── Send Callback ───────────────────── */

/**
 * @brief Callback used by the backend to send encrypted DTLS records
 *        out through the ICE transport.
 *
 * @param data  Encrypted data to send.
 * @param len   Length of data.
 * @param arg   User-provided context.
 * @return      xErrno_Ok on success.
 */
typedef xErrno (*xDtlsSendFn)(const uint8_t *data, size_t len, void *arg);

/* ───────────────────── Backend VTable ───────────────────── */

/**
 * @brief TLS backend interface (vtable).
 *
 * Each backend (OpenSSL, mbedTLS) provides a static instance of this
 * struct. The xDtlsTransport calls through these function pointers.
 */
typedef struct xDtlsBackend {
  /** Human-readable backend name (e.g. "openssl", "mbedtls"). */
  const char *name;

  /**
   * @brief Create backend context and generate a self-signed ECDSA
   *        P-256 certificate.
   *
   * @param role     DTLS role (active/passive).
   * @param send_fn  Callback for sending encrypted data.
   * @param send_arg Argument forwarded to send_fn.
   * @return         Opaque backend context, or NULL on failure.
   */
  xDtlsBackendCtx *(*create)(xDtlsRole role, xDtlsSendFn send_fn,
                              void *send_arg);

  /**
   * @brief Destroy backend context and free all resources.
   */
  void (*destroy)(xDtlsBackendCtx *ctx);

  /**
   * @brief Get the local certificate's SHA-256 fingerprint.
   *
   * @param ctx  Backend context.
   * @param out  Output buffer (XDTLS_FINGERPRINT_SIZE bytes).
   * @return     xErrno_Ok on success.
   */
  xErrno (*get_fingerprint)(xDtlsBackendCtx *ctx, uint8_t *out);

  /**
   * @brief Drive the DTLS handshake forward.
   *
   * Called after feeding input data or when the handshake timer fires.
   * The backend should call send_fn to emit any pending output.
   *
   * @param ctx  Backend context.
   * @return     xErrno_Ok if handshake is complete,
   *             xErrno_Again if more data is needed,
   *             other error on failure.
   */
  xErrno (*handshake)(xDtlsBackendCtx *ctx);

  /**
   * @brief Feed received DTLS record data into the backend.
   *
   * @param ctx   Backend context.
   * @param data  Raw DTLS record bytes from the network.
   * @param len   Length of data.
   * @return      xErrno_Ok on success.
   */
  xErrno (*feed_input)(xDtlsBackendCtx *ctx, const uint8_t *data, size_t len);

  /**
   * @brief Encrypt and send application data through DTLS.
   *
   * @param ctx   Backend context.
   * @param data  Plaintext application data.
   * @param len   Length of data.
   * @return      xErrno_Ok on success.
   */
  xErrno (*encrypt_send)(xDtlsBackendCtx *ctx, const uint8_t *data,
                         size_t len);

  /**
   * @brief Read decrypted application data from the backend.
   *
   * @param ctx      Backend context.
   * @param buf      Output buffer.
   * @param buf_cap  Buffer capacity.
   * @param out_len  Actual bytes read (output).
   * @return         xErrno_Ok on success, xErrno_Again if no data.
   */
  xErrno (*decrypt_read)(xDtlsBackendCtx *ctx, uint8_t *buf, size_t buf_cap,
                         size_t *out_len);

  /**
   * @brief Get the remote peer's certificate SHA-256 fingerprint.
   *
   * Only valid after handshake is complete.
   *
   * @param ctx  Backend context.
   * @param out  Output buffer (XDTLS_FINGERPRINT_SIZE bytes).
   * @return     xErrno_Ok on success.
   */
  xErrno (*get_remote_fingerprint)(xDtlsBackendCtx *ctx, uint8_t *out);

  /**
   * @brief Check if the DTLS handshake is complete.
   *
   * @param ctx  Backend context.
   * @return     true if handshake is finished.
   */
  bool (*is_handshake_done)(xDtlsBackendCtx *ctx);

} xDtlsBackend;

/* ───────────────────── Backend Registration ───────────────────── */

/**
 * @brief Get the compiled-in TLS backend.
 *
 * Returns the OpenSSL or mbedTLS backend depending on which was
 * selected at build time via XP2P_TLS_BACKEND.
 */
const xDtlsBackend *xDtlsBackendGet(void);

/* ───────────────────── Fingerprint Helpers ───────────────────── */

/**
 * @brief Format a raw fingerprint as a colon-separated hex string.
 *
 * @param raw  Raw fingerprint bytes (XDTLS_FINGERPRINT_SIZE).
 * @param out  Output string buffer (at least XDTLS_FINGERPRINT_STR_SIZE).
 */
static inline void xDtlsFingerprintToStr(const uint8_t *raw, char *out) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < XDTLS_FINGERPRINT_SIZE; i++) {
    if (i > 0) *out++ = ':';
    *out++ = hex[(raw[i] >> 4) & 0xF];
    *out++ = hex[raw[i] & 0xF];
  }
  *out = '\0';
}

/**
 * @brief Parse a colon-separated hex fingerprint string to raw bytes.
 *
 * @param str  Fingerprint string (e.g. "AA:BB:CC:...").
 * @param out  Output buffer (XDTLS_FINGERPRINT_SIZE bytes).
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xDtlsFingerprintFromStr(const char *str, uint8_t *out);

#endif /* XP2P_DTLS_BACKEND_H */
