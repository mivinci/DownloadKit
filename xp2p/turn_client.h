/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_client.h - TURN client (RFC 5766)
 */

#ifndef XP2P_TURN_CLIENT_H
#define XP2P_TURN_CLIENT_H

#include "stun_txn.h"
#include "turn_channel.h"

/** Maximum number of TURN permissions. */
#define XTURN_MAX_PERMISSIONS 16

/** Maximum number of TURN channel bindings. */
#define XTURN_MAX_CHANNELS 8

/**
 * @brief TURN client state.
 */
typedef enum {
  xTurnState_Idle = 0,
  xTurnState_Allocating,
  xTurnState_Allocated,
  xTurnState_Failed,
} xTurnState;

/**
 * @brief Callback when TURN allocation succeeds.
 *
 * @param relayed_addr  The relayed address (TURN server's allocation).
 * @param mapped_addr   The server-reflexive address.
 * @param lifetime      Allocation lifetime in seconds.
 * @param arg           User argument.
 */
typedef void (*xTurnOnAllocated)(const struct sockaddr *relayed_addr,
                                 const struct sockaddr *mapped_addr,
                                 uint32_t lifetime, void *arg);

/**
 * @brief Callback when TURN allocation fails.
 */
typedef void (*xTurnOnFailed)(xErrno err, void *arg);

/**
 * @brief Callback when data is received via TURN relay.
 */
typedef void (*xTurnOnData)(const uint8_t *data, size_t len,
                            const struct sockaddr *from, void *arg);

/**
 * @brief TURN client configuration.
 */
XDEF_STRUCT(xTurnConfig) {
  struct sockaddr_storage server;        /**< TURN server address.          */
  char                    username[128]; /**< Long-term credential username.*/
  char                    password[128]; /**< Long-term credential password.*/

  xStunTxnSendFunc send_fn;  /**< Send function.                */
  void            *send_arg; /**< Send function argument.       */

  xTurnOnAllocated on_allocated; /**< Allocation success callback.  */
  xTurnOnFailed    on_failed;    /**< Allocation failure callback.  */
  xTurnOnData      on_data;      /**< Data received callback.       */
  void            *callback_arg; /**< User argument for callbacks.  */
};

/**
 * @brief TURN client instance.
 */
XDEF_STRUCT(xTurnClient) {
  xTurnConfig config;

  xTurnState  state;
  xStunTxnMgr txn_mgr; /**< Transaction manager.          */
  xEventLoop  loop;    /**< Event loop.                   */

  /* Allocation state */
  struct sockaddr_storage relayed_addr;
  struct sockaddr_storage mapped_addr;
  uint32_t                lifetime;      /**< Allocation lifetime (seconds).*/
  xEventTimer             refresh_timer; /**< Refresh timer.                */

  /* Authentication */
  char realm[128];
  char nonce[256];
  bool has_credentials; /**< Got realm/nonce from 401.     */

  /* Permissions */
  struct sockaddr_storage permissions[XTURN_MAX_PERMISSIONS];
  int                     permission_count;

  /* Channel bindings */
  xTurnChannel channels[XTURN_MAX_CHANNELS];
  int          channel_count;
  uint16_t     next_channel; /**< Next channel number to assign.*/
};

/**
 * @brief Initialize a TURN client.
 */
void xTurnClientInit(xTurnClient *tc, xEventLoop loop,
                     const xTurnConfig *config);

/**
 * @brief Destroy a TURN client, releasing all resources.
 */
void xTurnClientDestroy(xTurnClient *tc);

/**
 * @brief Start TURN allocation.
 */
xErrno xTurnClientAllocate(xTurnClient *tc);

/**
 * @brief Create a permission for a peer address.
 */
xErrno xTurnClientCreatePermission(xTurnClient           *tc,
                                   const struct sockaddr *peer);

/**
 * @brief Bind a channel to a peer address.
 *
 * @param tc    TURN client.
 * @param peer  Peer address.
 * @return      Channel number on success, -1 on failure.
 */
int xTurnClientChannelBind(xTurnClient *tc, const struct sockaddr *peer);

/**
 * @brief Send data to a peer via TURN relay.
 *
 * Uses ChannelData if a channel is bound, otherwise Send Indication.
 */
xErrno xTurnClientSendData(xTurnClient *tc, const struct sockaddr *peer,
                           const uint8_t *data, size_t len);

/**
 * @brief Handle an incoming STUN message for the TURN client.
 *
 * @return true if the message was handled by the TURN client.
 */
bool xTurnClientOnMessage(xTurnClient *tc, const xStunMsg *msg,
                          const uint8_t *raw_buf, size_t raw_len,
                          const struct sockaddr *from);

/**
 * @brief Handle incoming ChannelData.
 */
bool xTurnClientOnChannelData(xTurnClient *tc, const uint8_t *buf, size_t len);

#endif /* XP2P_TURN_CLIENT_H */
