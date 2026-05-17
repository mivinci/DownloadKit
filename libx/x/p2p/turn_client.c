/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_client.c - TURN client (RFC 5766)
 */

#include "turn_client.h"

#include "ice_private.h"
#include "stun_attr.h"
#include "stun_msg.h"
#include <x/crypto/md5.h>

#include <x/base/log.h>

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Helpers ───────────────────── */

static size_t sockaddr_size(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) return sizeof(struct sockaddr_in);
  if (addr->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
  return 0;
}

/**
 * Compute the long-term credential key:
 *   key = MD5(username ":" realm ":" password)
 * per RFC 5389 §15.4.
 */
static void turn_compute_key(const xTurnClient *tc, uint8_t *key, size_t *key_len) {
  const char *user  = tc->config.username;
  const char *realm = tc->realm;
  const char *pass  = tc->config.password;
  size_t      ulen  = strlen(user);
  size_t      rlen  = strlen(realm);
  size_t      plen  = strlen(pass);

  if (rlen == 0) {
    /* No realm yet (first request) — use password directly */
    memcpy(key, pass, plen);
    *key_len = plen;
    return;
  }

  /* Concatenate "username:realm:password" */
  size_t   total = ulen + 1 + rlen + 1 + plen;
  uint8_t *buf   = (uint8_t *)malloc(total);
  if (!buf) {
    memcpy(key, pass, plen);
    *key_len = plen;
    return;
  }
  memcpy(buf, user, ulen);
  buf[ulen] = ':';
  memcpy(buf + ulen + 1, realm, rlen);
  buf[ulen + 1 + rlen] = ':';
  memcpy(buf + ulen + 1 + rlen + 1, pass, plen);

  xMd5(buf, total, key);
  *key_len = XCRYPTO_MD5_DIGEST_SIZE;
  free(buf);
}

/* ───────────────────── Init / Destroy ───────────────────── */

void xTurnClientInit(xTurnClient *tc, xEventLoop loop, const xTurnConfig *config) {
  memset(tc, 0, sizeof(*tc));
  tc->config       = *config;
  tc->loop         = loop;
  tc->state        = xTurnState_Idle;
  tc->next_channel = XTURN_CHANNEL_MIN;
  xStunTxnMgrInit(&tc->txn_mgr, loop);
}

void xTurnClientDestroy(xTurnClient *tc) {
  if (tc->refresh_timer) {
    xEventLoopTimerCancel(tc->loop, tc->refresh_timer);
    tc->refresh_timer = NULL;
  }
  xStunTxnMgrDestroy(&tc->txn_mgr);
  tc->state = xTurnState_Idle;
}

/* ───────────────────── Refresh ───────────────────── */

static void on_refresh_response(const xStunMsg        *msg,
                                const struct sockaddr *from __attribute__((unused)), void *arg);
static void schedule_refresh(xTurnClient *tc);

static void refresh_timer_cb(void *arg) {
  xTurnClient *tc   = (xTurnClient *)arg;
  tc->refresh_timer = NULL;

  if (tc->state != xTurnState_Allocated) return;

  /* Send Refresh Request */
  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_RefreshRequest, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE, sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  /* Request same lifetime */
  xStunAttrWriteLifetime(&w, tc->lifetime);

  if (tc->has_credentials) {
    xStunAttrWriteUsername(&w, tc->config.username, "");
    xStunAttrWriteRealm(&w, tc->realm);
    xStunAttrWriteNonce(&w, tc->nonce);

    uint8_t key[128];
    size_t  key_len;
    turn_compute_key(tc, key, &key_len);
    xStunAttrWriteMessageIntegrity(&w, msg_buf, key, key_len);
  }

  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  xStunTxnMgrSendRaw(&tc->txn_mgr, msg_buf, total, (struct sockaddr *)&tc->config.server,
                     tc->config.send_fn, tc->config.send_arg, on_refresh_response, tc);
}

static void on_refresh_response(const xStunMsg        *msg,
                                const struct sockaddr *from __attribute__((unused)), void *arg) {
  xTurnClient *tc = (xTurnClient *)arg;

  if (!msg || xStunMsgIsErrorResponse(msg->type)) {
    /* Refresh failed — allocation lost */
    tc->state = xTurnState_Failed;
    if (tc->config.on_failed) {
      tc->config.on_failed(msg ? xErrno_SysError : xErrno_Timeout, tc->config.ctx);
    }
    return;
  }

  /* Update lifetime from response */
  xStunAttrIter iter;
  xStunAttrIterInit(&iter, msg);
  xStunAttr attr;
  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_Lifetime && attr.length >= 4) {
      tc->lifetime = xReadU32BE(attr.value);
    }
  }

  /* Schedule next refresh */
  schedule_refresh(tc);
}

static void schedule_refresh(xTurnClient *tc) {
  if (tc->refresh_timer) {
    xEventLoopTimerCancel(tc->loop, tc->refresh_timer);
    tc->refresh_timer = NULL;
  }
  if (tc->lifetime == 0) return;

  /* Refresh at 80% of lifetime */
  uint32_t refresh_ms = (tc->lifetime * XTURN_REFRESH_RATIO / 100) * 1000;
  if (refresh_ms == 0) refresh_ms = 1000;

  tc->refresh_timer = xEventLoopTimerAfter(tc->loop, refresh_timer_cb, tc, refresh_ms);
}

/* ───────────────────── Allocate ───────────────────── */

static void on_allocate_response(const xStunMsg *msg, const struct sockaddr *from, void *arg);

static xErrno send_allocate(xTurnClient *tc, bool with_credentials) {
  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_AllocateRequest, txn_id);

  /* Write header */
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  /* Write attributes */
  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE, sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  xStunAttrWriteRequestedTransport(&w, XTURN_TRANSPORT_UDP);

  if (with_credentials && tc->has_credentials) {
    xStunAttrWriteUsername(&w, tc->config.username, "");
    xStunAttrWriteRealm(&w, tc->realm);
    xStunAttrWriteNonce(&w, tc->nonce);

    uint8_t key[128];
    size_t  key_len;
    turn_compute_key(tc, key, &key_len);
    xStunAttrWriteMessageIntegrity(&w, msg_buf, key, key_len);
  }

  /* Update length */
  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  return xStunTxnMgrSendRaw(&tc->txn_mgr, msg_buf, total, (struct sockaddr *)&tc->config.server,
                            tc->config.send_fn, tc->config.send_arg, on_allocate_response, tc);
}

static void on_allocate_response(const xStunMsg        *msg,
                                 const struct sockaddr *from __attribute__((unused)), void *arg) {
  xTurnClient *tc = (xTurnClient *)arg;

  if (!msg) {
    /* Timeout */
    tc->state = xTurnState_Failed;
    if (tc->config.on_failed) {
      tc->config.on_failed(xErrno_Timeout, tc->config.ctx);
    }
    return;
  }

  if (xStunMsgIsErrorResponse(msg->type)) {
    /* Check for 401 Unauthorized — need credentials */
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;

    int error_code = 0;
    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_ErrorCode) {
        xStunAttrDecodeErrorCode(&attr, &error_code, NULL, NULL);
      } else if (attr.type == xStunAttrType_Realm) {
        size_t len = attr.length < sizeof(tc->realm) - 1 ? attr.length : sizeof(tc->realm) - 1;
        memcpy(tc->realm, attr.value, len);
        tc->realm[len] = '\0';
      } else if (attr.type == xStunAttrType_Nonce) {
        size_t len = attr.length < sizeof(tc->nonce) - 1 ? attr.length : sizeof(tc->nonce) - 1;
        memcpy(tc->nonce, attr.value, len);
        tc->nonce[len] = '\0';
      }
    }

    if (error_code == 401 && !tc->has_credentials) {
      tc->has_credentials = true;
      send_allocate(tc, true);
      return;
    }

    tc->state = xTurnState_Failed;
    if (tc->config.on_failed) {
      tc->config.on_failed(xErrno_SysError, tc->config.ctx);
    }
    return;
  }

  /* Success response — extract RELAYED-ADDRESS and LIFETIME */
  xStunAttrIter iter;
  xStunAttrIterInit(&iter, msg);
  xStunAttr attr;

  bool got_relayed = false;
  bool got_mapped  = false;

  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_XorRelayedAddress) {
      xStunAttrDecodeXorMappedAddress(&attr, msg->txn_id,
                                      (struct sockaddr_storage *)&tc->relayed_addr);
      got_relayed = true;
    } else if (attr.type == xStunAttrType_XorMappedAddress) {
      xStunAttrDecodeXorMappedAddress(&attr, msg->txn_id,
                                      (struct sockaddr_storage *)&tc->mapped_addr);
      got_mapped = true;
    } else if (attr.type == xStunAttrType_Lifetime) {
      if (attr.length >= 4) {
        tc->lifetime = xReadU32BE(attr.value);
      }
    }
  }

  if (!got_relayed) {
    tc->state = xTurnState_Failed;
    if (tc->config.on_failed) {
      tc->config.on_failed(xErrno_InvalidArg, tc->config.ctx);
    }
    return;
  }

  tc->state = xTurnState_Allocated;

  /* Schedule refresh at 80% of lifetime */
  schedule_refresh(tc);

  if (tc->config.on_allocated) {
    tc->config.on_allocated((struct sockaddr *)&tc->relayed_addr,
                            got_mapped ? (struct sockaddr *)&tc->mapped_addr : NULL, tc->lifetime,
                            tc->config.ctx);
  }
}

xErrno xTurnClientAllocate(xTurnClient *tc) {
  if (tc->state != xTurnState_Idle) return xErrno_InvalidArg;
  tc->state = xTurnState_Allocating;
  return send_allocate(tc, false);
}

/* ───────────────────── CreatePermission ───────────────────── */

static void on_permission_response(const xStunMsg        *msg,
                                   const struct sockaddr *from __attribute__((unused)), void *arg) {
  (void)arg;
  if (!msg) {
    XDEBUG("[turn] CreatePermission timed out");
    return;
  }
  if (xStunMsgIsErrorResponse(msg->type)) {
    int           error_code = 0;
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;
    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_ErrorCode) {
        xStunAttrDecodeErrorCode(&attr, &error_code, NULL, NULL);
      }
    }
    XDEBUG("[turn] CreatePermission failed, error=%d", error_code);
    return;
  }
  XDEBUG("[turn] CreatePermission succeeded");
}

xErrno xTurnClientCreatePermission(xTurnClient *tc, const struct sockaddr *peer) {
  if (tc->state != xTurnState_Allocated) return xErrno_InvalidArg;
  if (tc->permission_count >= XTURN_MAX_PERMISSIONS) return xErrno_NoMemory;

  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_CreatePermissionRequest, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE, sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  xStunAttrWriteXorPeerAddress(&w, peer, txn_id);

  if (tc->has_credentials) {
    xStunAttrWriteUsername(&w, tc->config.username, "");
    xStunAttrWriteRealm(&w, tc->realm);
    xStunAttrWriteNonce(&w, tc->nonce);

    uint8_t key[128];
    size_t  key_len;
    turn_compute_key(tc, key, &key_len);
    xStunAttrWriteMessageIntegrity(&w, msg_buf, key, key_len);
  }

  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  /* Store permission */
  size_t sz = sockaddr_size(peer);
  if (sz > 0) {
    memcpy(&tc->permissions[tc->permission_count], peer, sz);
    tc->permission_count++;
  }

  return xStunTxnMgrSendRaw(&tc->txn_mgr, msg_buf, total, (struct sockaddr *)&tc->config.server,
                            tc->config.send_fn, tc->config.send_arg, on_permission_response, tc);
}

/* ───────────────────── ChannelBind ───────────────────── */

static void on_channel_bind_response(const xStunMsg        *msg,
                                     const struct sockaddr *from __attribute__((unused)),
                                     void                  *arg) {
  (void)arg;
  if (!msg) {
    XDEBUG("[turn] ChannelBind timed out");
    return;
  }
  if (xStunMsgIsErrorResponse(msg->type)) {
    int           error_code = 0;
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;
    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_ErrorCode) {
        xStunAttrDecodeErrorCode(&attr, &error_code, NULL, NULL);
      }
    }
    XDEBUG("[turn] ChannelBind failed, error=%d", error_code);
    return;
  }
  XDEBUG("[turn] ChannelBind succeeded");
}

int xTurnClientChannelBind(xTurnClient *tc, const struct sockaddr *peer) {
  if (tc->state != xTurnState_Allocated) return -1;
  if (tc->channel_count >= XTURN_MAX_CHANNELS) return -1;
  if (tc->next_channel > XTURN_CHANNEL_MAX) return -1;

  uint16_t ch = tc->next_channel++;

  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_ChannelBindRequest, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE, sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  xStunAttrWriteChannelNumber(&w, ch);
  xStunAttrWriteXorPeerAddress(&w, peer, txn_id);

  if (tc->has_credentials) {
    xStunAttrWriteUsername(&w, tc->config.username, "");
    xStunAttrWriteRealm(&w, tc->realm);
    xStunAttrWriteNonce(&w, tc->nonce);

    uint8_t key[128];
    size_t  key_len;
    turn_compute_key(tc, key, &key_len);
    xStunAttrWriteMessageIntegrity(&w, msg_buf, key, key_len);
  }

  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  /* Store channel binding */
  xTurnChannel *binding = &tc->channels[tc->channel_count++];
  binding->number       = ch;
  size_t sz             = sockaddr_size(peer);
  if (sz > 0) memcpy(&binding->peer, peer, sz);
  binding->bound = true;

  xStunTxnMgrSendRaw(&tc->txn_mgr, msg_buf, total, (struct sockaddr *)&tc->config.server,
                     tc->config.send_fn, tc->config.send_arg, on_channel_bind_response, tc);

  return (int)ch;
}

/* ───────────────────── Send Data ───────────────────── */

static int find_channel_for_peer(xTurnClient *tc, const struct sockaddr *peer) {
  for (int i = 0; i < tc->channel_count; i++) {
    if (!tc->channels[i].bound) continue;
    /* Compare addresses */
    const struct sockaddr *ch_peer = (const struct sockaddr *)&tc->channels[i].peer;
    if (ch_peer->sa_family != peer->sa_family) continue;

    if (peer->sa_family == AF_INET) {
      const struct sockaddr_in *a = (const struct sockaddr_in *)peer;
      const struct sockaddr_in *b = (const struct sockaddr_in *)ch_peer;
      if (a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr) {
        return (int)tc->channels[i].number;
      }
    } else if (peer->sa_family == AF_INET6) {
      const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)peer;
      const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)ch_peer;
      if (a->sin6_port == b->sin6_port && memcmp(&a->sin6_addr, &b->sin6_addr, 16) == 0) {
        return (int)tc->channels[i].number;
      }
    }
  }
  return -1;
}

xErrno xTurnClientSendData(xTurnClient *tc, const struct sockaddr *peer, const uint8_t *data,
                           size_t len) {
  if (tc->state != xTurnState_Allocated) return xErrno_InvalidArg;

  int ch = find_channel_for_peer(tc, peer);
  if (ch >= 0) {
    /* Use ChannelData — more efficient */
    uint8_t buf[XSTUN_MAX_MSG_SIZE];
    int     encoded = xTurnChannelDataEncode((uint16_t)ch, data, (uint16_t)len, buf, sizeof(buf));
    if (encoded < 0) return xErrno_NoMemory;
    return tc->config.send_fn(buf, (size_t)encoded, (struct sockaddr *)&tc->config.server,
                              tc->config.send_arg);
  }

  /* Fallback: Send Indication */
  uint8_t msg_buf[XSTUN_MAX_MSG_SIZE];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
  memset(txn_id, 0, XSTUN_TXN_ID_SIZE);

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_SendIndication, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE, sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  xStunAttrWriteXorPeerAddress(&w, peer, txn_id);
  xStunAttrWriteData(&w, data, len);

  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  return tc->config.send_fn(msg_buf, total, (struct sockaddr *)&tc->config.server,
                            tc->config.send_arg);
}

/* ───────────────────── Message Handling ───────────────────── */

bool xTurnClientOnMessage(xTurnClient *tc, const xStunMsg *msg, const uint8_t *raw_buf,
                          size_t raw_len, const struct sockaddr *from) {
  /* First try to match to a pending transaction */
  if (xStunMsgIsSuccessResponse(msg->type) || xStunMsgIsErrorResponse(msg->type)) {
    return xStunTxnMgrOnResponse(&tc->txn_mgr, msg, raw_buf, raw_len, from);
  }

  /* Handle Data Indication */
  if (msg->type == xStunMsgType_DataIndication) {
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;

    struct sockaddr_storage peer_addr;
    const uint8_t          *ind_data     = NULL;
    uint16_t                ind_data_len = 0;
    bool                    got_peer     = false;

    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_XorPeerAddress) {
        xStunAttrDecodeXorMappedAddress(&attr, msg->txn_id, &peer_addr);
        got_peer = true;
      } else if (attr.type == xStunAttrType_Data) {
        ind_data     = attr.value;
        ind_data_len = attr.length;
      }
    }

    if (got_peer && ind_data && tc->config.on_data) {
      tc->config.on_data(ind_data, ind_data_len, (struct sockaddr *)&peer_addr, tc->config.ctx);
    }
    return true;
  }

  return false;
}

bool xTurnClientOnChannelData(xTurnClient *tc, const uint8_t *buf, size_t len) {
  uint16_t       channel;
  const uint8_t *data;
  uint16_t       data_len;

  if (xTurnChannelDataDecode(buf, len, &channel, &data, &data_len) != xErrno_Ok) {
    return false;
  }

  /* Find the peer for this channel */
  for (int i = 0; i < tc->channel_count; i++) {
    if (tc->channels[i].number == channel && tc->channels[i].bound) {
      if (tc->config.on_data) {
        tc->config.on_data(data, data_len, (struct sockaddr *)&tc->channels[i].peer,
                           tc->config.ctx);
      }
      return true;
    }
  }

  return false;
}
