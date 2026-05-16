/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_transport.c - DTLS transport core logic (backend-agnostic)
 */

#include "dtls_transport.h"
#include "dtls_backend.h"

#include <x/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Default Timeout ───────────────────── */

#define XDTLS_DEFAULT_HANDSHAKE_TIMEOUT_MS 10000

/* ───────────────────── Internal Structure ───────────────────── */

XDEF_STRUCT(xDtlsTransport_) {
  xDtlsTransportConf conf;
  xDtlsState         state;
  xDtlsRole          effective_role; /**< Resolved role (Active or Passive). */

  const xDtlsBackend *backend;
  xDtlsBackendCtx    *backend_ctx;

  uint8_t local_fingerprint[XDTLS_FINGERPRINT_SIZE];

  xEventTimer handshake_timer;
  bool        driving;          /**< Re-entrancy guard for drive_handshake. */
  bool        feed_while_driving; /**< Data arrived while driving. */
};

/* ───────────────────── Helpers ───────────────────── */

static void set_state(xDtlsTransport_ *t, xDtlsState new_state) {
  if (t->state == new_state) return;
  t->state = new_state;
  if (t->conf.on_state_change) {
    t->conf.on_state_change((xDtlsTransport)t, new_state, t->conf.ctx);
  }
}

static void handshake_timeout_cb(void *arg) {
  xDtlsTransport_ *t = (xDtlsTransport_ *)arg;
  t->handshake_timer  = NULL;

  if (t->state == xDtlsState_Connecting) {
    set_state(t, xDtlsState_Failed);
  }
}

/**
 * @brief Try to read decrypted data from the backend and deliver
 *        it via the on_data callback.
 */
static void drain_decrypted(xDtlsTransport_ *t) {
  uint8_t buf[4096];
  size_t  out_len = 0;

  while (t->backend->decrypt_read(t->backend_ctx, buf, sizeof(buf),
                                  &out_len) == xErrno_Ok &&
         out_len > 0) {
    if (t->conf.on_data) {
      t->conf.on_data((xDtlsTransport)t, buf, out_len, t->conf.ctx);
    }
    out_len = 0;
  }
}

/**
 * @brief Drive the handshake and check for completion.
 */
static void drive_handshake(xDtlsTransport_ *t) {
  if (t->state != xDtlsState_Connecting) return;

  /*
   * Re-entrancy guard.  Both OpenSSL and mbedTLS may invoke the send
   * callback synchronously during handshake() / flush.  In a loopback
   * test the send callback feeds data directly into the peer, which
   * calls FeedInput → drive_handshake on the peer, and the peer's
   * send callback feeds back into *us*, causing unbounded recursion.
   *
   * When `driving` is true, FeedInput still writes data into the
   * backend buffer and sets `feed_while_driving` so the loop below
   * knows to retry instead of stopping at Again.
   */
  if (t->driving) return;
  t->driving = true;

  for (;;) {
    t->feed_while_driving = false;
    xErrno err = t->backend->handshake(t->backend_ctx);

    /*
     * Flush any buffered output (e.g. OpenSSL memory BIO).  This is
     * done while `driving` is still true so that any recursive
     * FeedInput triggered by the send callback only writes data and
     * sets feed_while_driving, without re-entering drive_handshake.
     */
    if (t->backend->flush_output) {
      t->backend->flush_output(t->backend_ctx);
    }

    if (err == xErrno_Ok) {
      /* Handshake complete — verify remote fingerprint if requested */
      if (t->conf.verify_fingerprint) {
        uint8_t remote_fp[XDTLS_FINGERPRINT_SIZE];
        xErrno  fp_err =
          t->backend->get_remote_fingerprint(t->backend_ctx, remote_fp);
        if (fp_err != xErrno_Ok ||
            memcmp(remote_fp, t->conf.remote_fingerprint,
                   XDTLS_FINGERPRINT_SIZE) != 0) {
          set_state(t, xDtlsState_Failed);
          t->driving = false;
          return;
        }
      }

      /* Cancel handshake timer */
      if (t->handshake_timer) {
        xEventLoopTimerCancel(t->conf.loop, t->handshake_timer);
        t->handshake_timer = NULL;
      }

      set_state(t, xDtlsState_Connected);
      t->driving = false;
      return;
    }

    if (err == xErrno_Again) {
      /*
       * If new data arrived while we were inside handshake() (via the
       * send-callback → peer-FeedInput → peer-handshake → peer-send
       * → our-FeedInput path), retry immediately instead of waiting
       * for the next external FeedInput call.
       */
      if (t->feed_while_driving) continue;

      /* No new data — stop driving */
      t->driving = false;
      return;
    }

    /* Any other error — handshake failed */
    set_state(t, xDtlsState_Failed);
    t->driving = false;
    return;
  }
}

/* ───────────────────── Fingerprint String Parser ───────────────────── */

xErrno xDtlsFingerprintFromStr(const char *str, uint8_t *out) {
  if (!str || !out) return xErrno_InvalidArg;

  int idx = 0;
  for (const char *p = str; *p && idx < XDTLS_FINGERPRINT_SIZE; p++) {
    if (*p == ':') continue;

    uint8_t hi, lo;
    if (*p >= '0' && *p <= '9')
      hi = (uint8_t)(*p - '0');
    else if (*p >= 'A' && *p <= 'F')
      hi = (uint8_t)(*p - 'A' + 10);
    else if (*p >= 'a' && *p <= 'f')
      hi = (uint8_t)(*p - 'a' + 10);
    else
      return xErrno_InvalidArg;

    p++;
    if (!*p) return xErrno_InvalidArg;

    if (*p >= '0' && *p <= '9')
      lo = (uint8_t)(*p - '0');
    else if (*p >= 'A' && *p <= 'F')
      lo = (uint8_t)(*p - 'A' + 10);
    else if (*p >= 'a' && *p <= 'f')
      lo = (uint8_t)(*p - 'a' + 10);
    else
      return xErrno_InvalidArg;

    out[idx++] = (uint8_t)((hi << 4) | lo);
  }

  if (idx != XDTLS_FINGERPRINT_SIZE) return xErrno_InvalidArg;
  return xErrno_Ok;
}

/* ───────────────────── Public API ───────────────────── */

xDtlsTransport xDtlsTransportCreate(const xDtlsTransportConf *conf) {
  if (!conf || !conf->loop || !conf->send_fn) return NULL;

  const xDtlsBackend *backend = xDtlsBackendGet();
  if (!backend) return NULL;

  xDtlsTransport_ *t = (xDtlsTransport_ *)calloc(1, sizeof(xDtlsTransport_));
  if (!t) return NULL;

  t->conf    = *conf;
  t->state   = xDtlsState_New;
  t->backend = backend;

  if (t->conf.handshake_timeout_ms == 0) {
    t->conf.handshake_timeout_ms = XDTLS_DEFAULT_HANDSHAKE_TIMEOUT_MS;
  }

  /* Determine effective role */
  xDtlsRole effective_role = conf->role;
  if (effective_role == xDtlsRole_Actpass) {
    /* Default to passive when actpass (answerer becomes passive) */
    effective_role = xDtlsRole_Passive;
  }
  t->effective_role = effective_role;

  /* Create backend context (generates self-signed cert) */
  t->backend_ctx =
    backend->create(effective_role, conf->send_fn, conf->send_arg);
  if (!t->backend_ctx) {
    free(t);
    return NULL;
  }

  /* Cache local fingerprint */
  if (backend->get_fingerprint(t->backend_ctx, t->local_fingerprint) !=
      xErrno_Ok) {
    backend->destroy(t->backend_ctx);
    free(t);
    return NULL;
  }

  return (xDtlsTransport)t;
}

void xDtlsTransportDestroy(xDtlsTransport transport) {
  if (!transport) return;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  if (t->handshake_timer) {
    xEventLoopTimerCancel(t->conf.loop, t->handshake_timer);
    t->handshake_timer = NULL;
  }

  if (t->backend_ctx) {
    t->backend->destroy(t->backend_ctx);
    t->backend_ctx = NULL;
  }

  set_state(t, xDtlsState_Closed);
  free(t);
}

xErrno xDtlsTransportStart(xDtlsTransport transport) {
  if (!transport) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  if (t->state != xDtlsState_New) return xErrno_InvalidArg;

  set_state(t, xDtlsState_Connecting);

  /* Start handshake timeout */
  t->handshake_timer = xEventLoopTimerAfter(
    t->conf.loop, handshake_timeout_cb, t, t->conf.handshake_timeout_ms);

  /* Drive the handshake (for active role, this sends ClientHello) */
  drive_handshake(t);

  return xErrno_Ok;
}

xErrno xDtlsTransportFeedInput(xDtlsTransport transport, const uint8_t *data,
                                size_t len) {
  if (!transport || !data || len == 0) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  /* Feed data into backend */
  xErrno err = t->backend->feed_input(t->backend_ctx, data, len);
  if (err != xErrno_Ok) return err;

  /* Signal the driving loop that new data is available */
  if (t->driving) {
    t->feed_while_driving = true;
  }

  /* Drive handshake if still connecting */
  if (t->state == xDtlsState_Connecting) {
    drive_handshake(t);
  }

  /* Try to read decrypted application data */
  if (t->state == xDtlsState_Connected) {
    drain_decrypted(t);
  }

  return xErrno_Ok;
}

xErrno xDtlsTransportSend(xDtlsTransport transport, const uint8_t *data,
                           size_t len) {
  if (!transport || !data || len == 0) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  if (t->state != xDtlsState_Connected) return xErrno_InvalidArg;

  return t->backend->encrypt_send(t->backend_ctx, data, len);
}

xErrno xDtlsTransportGetFingerprint(xDtlsTransport transport, uint8_t *out) {
  if (!transport || !out) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  memcpy(out, t->local_fingerprint, XDTLS_FINGERPRINT_SIZE);
  return xErrno_Ok;
}

xErrno xDtlsTransportGetFingerprintStr(xDtlsTransport transport, char *out) {
  if (!transport || !out) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  xDtlsFingerprintToStr(t->local_fingerprint, out);
  return xErrno_Ok;
}

xDtlsState xDtlsTransportGetState(xDtlsTransport transport) {
  if (!transport) return xDtlsState_Closed;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;
  return t->state;
}

xDtlsRole xDtlsTransportGetRole(xDtlsTransport transport) {
  if (!transport) return xDtlsRole_Passive;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;
  return t->effective_role;
}

xErrno xDtlsTransportSetRole(xDtlsTransport transport, xDtlsRole role) {
  if (!transport) return xErrno_InvalidArg;
  xDtlsTransport_ *t = (xDtlsTransport_ *)transport;

  /* Can only change role before the handshake starts */
  if (t->state != xDtlsState_New) return xErrno_InvalidArg;

  xDtlsRole effective = role;
  if (effective == xDtlsRole_Actpass) {
    effective = xDtlsRole_Passive;
  }

  if (effective == t->effective_role) return xErrno_Ok;

  /* Use backend set_role to rebuild SSL without regenerating the cert */
  xErrno err = t->backend->set_role(t->backend_ctx, effective);
  if (err != xErrno_Ok) return err;

  t->effective_role = effective;
  return xErrno_Ok;
}


