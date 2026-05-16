/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_txn.h - STUN transaction management (RFC 5389 §7.2.1)
 *
 * Manages request/response matching via transaction ID, with
 * exponential-backoff retransmission for unreliable (UDP) transport.
 */

#ifndef XP2P_STUN_TXN_H
#define XP2P_STUN_TXN_H

#include "ice_private.h"
#include "stun_msg.h"

#include <netinet/in.h>

/* ───────────────────── Transaction Callback ───────────────────── */

/**
 * @brief Callback invoked when a STUN transaction completes.
 *
 * @param msg   Decoded response message (NULL on timeout).
 * @param addr  Source address of the response (NULL on timeout).
 * @param arg   User-provided argument.
 */
typedef void (*xStunTxnFunc)(const xStunMsg *msg, const struct sockaddr *addr,
                             void *arg);

/**
 * @brief Callback for sending a STUN message over the network.
 *
 * @param data  Encoded STUN message bytes.
 * @param len   Length of data.
 * @param addr  Destination address.
 * @param arg   User-provided argument (typically the socket context).
 * @return      xErrno_Ok on success.
 */
typedef xErrno (*xStunTxnSendFunc)(const uint8_t *data, size_t len,
                                   const struct sockaddr *addr, void *arg);

/* ───────────────────── Transaction ───────────────────── */

/**
 * @brief A single STUN transaction.
 */
XDEF_STRUCT(xStunTxn) {
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];   /**< Transaction ID.              */
  uint8_t msg_buf[XSTUN_MAX_MSG_SIZE]; /**< Encoded request message.   */
  size_t  msg_len;                     /**< Length of encoded message.  */

  struct sockaddr_storage dest; /**< Destination address.        */

  xStunTxnFunc on_complete; /**< Completion callback.        */
  void        *ctx;         /**< User argument for callback. */

  xStunTxnSendFunc send_fn;  /**< Send function.              */
  void            *send_arg; /**< User argument for send.     */

  xEventLoop  loop;  /**< Event loop for timers.      */
  xEventTimer timer; /**< Retransmission timer.       */

  int      retransmit_count; /**< Current retransmit count.   */
  uint32_t rto_ms;           /**< Current RTO in milliseconds. */
  bool     cancelled;        /**< Transaction was cancelled.  */
};

/* ───────────────────── Transaction Manager ───────────────────── */

/** Maximum number of concurrent transactions. */
#define XSTUN_TXN_MAX 32

/**
 * @brief Transaction manager — tracks pending STUN transactions.
 */
XDEF_STRUCT(xStunTxnMgr) {
  xStunTxn  *txns[XSTUN_TXN_MAX]; /**< Pending transactions.         */
  int        count;               /**< Number of active transactions. */
  xEventLoop loop;                /**< Event loop.                    */
};

/**
 * @brief Initialize a transaction manager.
 *
 * @param mgr   Manager to initialize.
 * @param loop  Event loop for timers.
 */
void xStunTxnMgrInit(xStunTxnMgr *mgr, xEventLoop loop);

/**
 * @brief Clean up a transaction manager, cancelling all pending transactions.
 */
void xStunTxnMgrDestroy(xStunTxnMgr *mgr);

/**
 * @brief Send a STUN request and start a transaction.
 *
 * Generates a random transaction ID, encodes the message, sends it,
 * and starts the retransmission timer.
 *
 * @param mgr         Transaction manager.
 * @param msg_type    STUN message type (must be a request).
 * @param attrs       Encoded attribute data (may be NULL if attrs_len=0).
 * @param attrs_len   Length of attribute data.
 * @param dest        Destination address.
 * @param send_fn     Function to send the encoded message.
 * @param send_arg    Argument for send_fn.
 * @param on_complete Callback on success or timeout.
 * @param cb_arg      Argument for on_complete.
 * @return            xErrno_Ok on success.
 */
xErrno xStunTxnMgrSend(xStunTxnMgr *mgr, xStunMsgType msg_type,
                       const uint8_t *attrs, uint16_t attrs_len,
                       const struct sockaddr *dest, xStunTxnSendFunc send_fn,
                       void *send_arg, xStunTxnFunc on_complete, void *cb_arg);

/**
 * @brief Send a pre-built STUN request and start a transaction.
 *
 * The transaction ID is taken from the provided message buffer.
 *
 * @param mgr         Transaction manager.
 * @param msg_buf     Pre-encoded STUN message.
 * @param msg_len     Length of the message.
 * @param dest        Destination address.
 * @param send_fn     Function to send the encoded message.
 * @param send_arg    Argument for send_fn.
 * @param on_complete Callback on success or timeout.
 * @param cb_arg      Argument for on_complete.
 * @return            xErrno_Ok on success.
 */
xErrno xStunTxnMgrSendRaw(xStunTxnMgr *mgr, const uint8_t *msg_buf,
                          size_t msg_len, const struct sockaddr *dest,
                          xStunTxnSendFunc send_fn, void *send_arg,
                          xStunTxnFunc on_complete, void *cb_arg);

/**
 * @brief Handle an incoming STUN response.
 *
 * Matches the response to a pending transaction by transaction ID.
 * If matched, cancels the retransmission timer and invokes the callback.
 * If no match, the response is silently discarded.
 *
 * @param mgr      Transaction manager.
 * @param msg      Decoded STUN response message.
 * @param raw_buf  Raw message buffer (for integrity verification).
 * @param raw_len  Length of raw buffer.
 * @param from     Source address of the response.
 * @return         true if the response was matched to a transaction.
 */
bool xStunTxnMgrOnResponse(xStunTxnMgr *mgr, const xStunMsg *msg,
                           const uint8_t *raw_buf, size_t raw_len,
                           const struct sockaddr *from);

/**
 * @brief Cancel all pending transactions.
 */
void xStunTxnMgrCancelAll(xStunTxnMgr *mgr);

#endif /* XP2P_STUN_TXN_H */
