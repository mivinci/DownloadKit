/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_transport.c - DTLS transport core logic (backend-agnostic)
 */

#include "dtls_transport.h"
#include "dtls_backend.h"

#include <xbase/log.h>

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
   * Loop until the backend returns Again (needs more network data) or
   * the handshake completes/fails.  This is necessary because the
   * recv iobuf may contain multiple DTLS records (e.g. retransmitted
   * ClientHellos that arrived before we started the handshake).  Each
   * call to handshake() typically consumes one record, so we must
   * keep driving until the buffer is drained.
   */
  for (;;) {
    xErrno err = t->backend->handshake(t->backend_ctx);
    XDEBUG("[dtls] drive_handshake: result=%d (0=ok, 1=again)", (int)err);

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
          return;
        }
      }

      /* Cancel handshake timer */
      if (t->handshake_timer) {
        xEventLoopTimerCancel(t->conf.loop, t->handshake_timer);
        t->handshake_timer = NULL;
      }

      set_state(t, xDtlsState_Connected);
      return;
    }

    if (err == xErrno_Again) {
      /* Needs more network data — stop driving */
      return;
    }

    /* Any other error — handshake failed */
    set_state(t, xDtlsState_Failed);
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


