/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_txn.c - STUN transaction management (RFC 5389 §7.2.1)
 */

#include "stun_txn.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ───────────────────── Helpers ───────────────────── */

static size_t sockaddr_len(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) return sizeof(struct sockaddr_in);
  if (addr->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
  return 0;
}

static void generate_txn_id(uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  /* Use arc4random if available (macOS/BSD), otherwise fallback */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  static int seeded = 0;
  if (!seeded) {
    srand((unsigned)time(NULL));
    seeded = 1;
  }
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++) {
    txn_id[i] = (uint8_t)(rand() & 0xFF);
  }
#endif
}

/* ───────────────────── Timer Callback ───────────────────── */

static void txn_retransmit(void *arg);

static void txn_schedule_retransmit(xStunTxn *txn) {
  txn->timer =
    xEventLoopTimerAfter(txn->loop, txn_retransmit, txn, txn->rto_ms);
}

static void txn_retransmit(void *arg) {
  xStunTxn *txn = (xStunTxn *)arg;
  txn->timer    = NULL;

  if (txn->cancelled) return;

  if (txn->retransmit_count >= XSTUN_MAX_RETRANSMITS) {
    /* Timeout — invoke callback with NULL */
    xStunTxnFunc cb     = txn->on_complete;
    void        *cb_arg = txn->ctx;
    txn->cancelled      = true;
    if (cb) cb(NULL, NULL, cb_arg);
    return;
  }

  /* Retransmit */
  txn->send_fn(txn->msg_buf, txn->msg_len, (const struct sockaddr *)&txn->dest,
               txn->send_arg);
  txn->retransmit_count++;
  txn->rto_ms *= 2; /* Exponential backoff */
  txn_schedule_retransmit(txn);
}

/* ───────────────────── Manager ───────────────────── */

void xStunTxnMgrInit(xStunTxnMgr *mgr, xEventLoop loop) {
  memset(mgr, 0, sizeof(*mgr));
  mgr->loop = loop;
}

static void txn_free(xStunTxnMgr *mgr, int index) {
  xStunTxn *txn = mgr->txns[index];
  if (txn->timer) {
    xEventLoopTimerCancel(mgr->loop, txn->timer);
    txn->timer = NULL;
  }
  /* Notify callback so it can release cb_arg (e.g. CheckCtx).
   * Pass NULL msg to signal cancellation/timeout. */
  if (!txn->cancelled && txn->on_complete) {
    txn->cancelled = true;
    txn->on_complete(NULL, NULL, txn->ctx);
  }
  /* Compact array */
  mgr->count--;
  if (index < mgr->count) {
    mgr->txns[index] = mgr->txns[mgr->count];
  }
  mgr->txns[mgr->count] = NULL;
  free(txn);
}

void xStunTxnMgrDestroy(xStunTxnMgr *mgr) {
  xStunTxnMgrCancelAll(mgr);
}

void xStunTxnMgrCancelAll(xStunTxnMgr *mgr) {
  while (mgr->count > 0) {
    txn_free(mgr, 0);
  }
}

static int txn_find(xStunTxnMgr *mgr, const uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  for (int i = 0; i < mgr->count; i++) {
    if (memcmp(mgr->txns[i]->txn_id, txn_id, XSTUN_TXN_ID_SIZE) == 0) {
      return i;
    }
  }
  return -1;
}

/* ───────────────────── Send ───────────────────── */

xErrno xStunTxnMgrSend(xStunTxnMgr *mgr, xStunMsgType msg_type,
                       const uint8_t *attrs, uint16_t attrs_len,
                       const struct sockaddr *dest, xStunTxnSendFunc send_fn,
                       void *send_arg, xStunTxnFunc on_complete, void *cb_arg) {
  if (!mgr || !dest || !send_fn) return xErrno_InvalidArg;
  if (mgr->count >= XSTUN_TXN_MAX) return xErrno_NoMemory;

  xStunTxn *txn = (xStunTxn *)calloc(1, sizeof(xStunTxn));
  if (!txn) return xErrno_NoMemory;

  generate_txn_id(txn->txn_id);

  /* Build the STUN message */
  xStunMsg msg;
  xStunMsgInit(&msg, msg_type, txn->txn_id);
  msg.attrs     = attrs;
  msg.attrs_len = attrs_len;

  int encoded = xStunMsgEncode(&msg, txn->msg_buf, sizeof(txn->msg_buf));
  if (encoded < 0) {
    free(txn);
    return xErrno_NoMemory;
  }
  txn->msg_len = (size_t)encoded;

  /* Store destination */
  size_t addr_len = sockaddr_len(dest);
  if (addr_len == 0) {
    free(txn);
    return xErrno_InvalidArg;
  }
  memcpy(&txn->dest, dest, addr_len);

  txn->on_complete      = on_complete;
  txn->ctx              = cb_arg;
  txn->send_fn          = send_fn;
  txn->send_arg         = send_arg;
  txn->loop             = mgr->loop;
  txn->retransmit_count = 0;
  txn->rto_ms           = XSTUN_INITIAL_RTO_MS;
  txn->cancelled        = false;

  /* Send the first request */
  xErrno err = send_fn(txn->msg_buf, txn->msg_len, dest, send_arg);
  if (err != xErrno_Ok) {
    free(txn);
    return err;
  }

  /* Start retransmission timer */
  txn_schedule_retransmit(txn);

  mgr->txns[mgr->count++] = txn;
  return xErrno_Ok;
}

xErrno xStunTxnMgrSendRaw(xStunTxnMgr *mgr, const uint8_t *msg_buf,
                          size_t msg_len, const struct sockaddr *dest,
                          xStunTxnSendFunc send_fn, void *send_arg,
                          xStunTxnFunc on_complete, void *cb_arg) {
  if (!mgr || !msg_buf || !dest || !send_fn) return xErrno_InvalidArg;
  if (msg_len < XSTUN_HEADER_SIZE || msg_len > XSTUN_MAX_MSG_SIZE) {
    return xErrno_InvalidArg;
  }
  if (mgr->count >= XSTUN_TXN_MAX) return xErrno_NoMemory;

  xStunTxn *txn = (xStunTxn *)calloc(1, sizeof(xStunTxn));
  if (!txn) return xErrno_NoMemory;

  /* Extract txn_id from the message */
  memcpy(txn->txn_id, msg_buf + 8, XSTUN_TXN_ID_SIZE);
  memcpy(txn->msg_buf, msg_buf, msg_len);
  txn->msg_len = msg_len;

  size_t addr_len = sockaddr_len(dest);
  if (addr_len == 0) {
    free(txn);
    return xErrno_InvalidArg;
  }
  memcpy(&txn->dest, dest, addr_len);

  txn->on_complete      = on_complete;
  txn->ctx              = cb_arg;
  txn->send_fn          = send_fn;
  txn->send_arg         = send_arg;
  txn->loop             = mgr->loop;
  txn->retransmit_count = 0;
  txn->rto_ms           = XSTUN_INITIAL_RTO_MS;
  txn->cancelled        = false;

  xErrno err = send_fn(txn->msg_buf, txn->msg_len, dest, send_arg);
  if (err != xErrno_Ok) {
    free(txn);
    return err;
  }

  txn_schedule_retransmit(txn);
  mgr->txns[mgr->count++] = txn;
  return xErrno_Ok;
}

/* ───────────────────── Response Handling ───────────────────── */

bool xStunTxnMgrOnResponse(xStunTxnMgr *mgr, const xStunMsg *msg,
                           const uint8_t *raw_buf __attribute__((unused)),
                           size_t         raw_len __attribute__((unused)),
                           const struct sockaddr *from) {
  if (!mgr || !msg) return false;

  int idx = txn_find(mgr, msg->txn_id);
  if (idx < 0) return false; /* No matching transaction — silently discard */

  xStunTxn *txn = mgr->txns[idx];

  /* Cancel retransmission timer */
  if (txn->timer) {
    xEventLoopTimerCancel(mgr->loop, txn->timer);
    txn->timer = NULL;
  }

  /* Invoke callback */
  xStunTxnFunc cb     = txn->on_complete;
  void        *cb_arg = txn->ctx;
  txn->cancelled      = true;

  /* Remove from manager before callback (callback may re-enter) */
  txn_free(mgr, idx);

  if (cb) cb(msg, from, cb_arg);
  return true;
}
