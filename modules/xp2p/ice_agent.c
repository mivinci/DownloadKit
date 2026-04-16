/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_agent.c - ICE Agent core state machine and public API
 */

#include "ice_agent.h"
#include "ice_candidate.h"
#include "ice_pair.h"
#include "ice_private.h"
#include "sdp.h"
#include "stun_attr.h"
#include "stun_msg.h"
#include "stun_txn.h"
#include "turn_client.h"

#include <xnet/dns.h>

#include <xbase/log.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* ───────────────────── Internal Agent Structure ───────────────────── */

XDEF_STRUCT(xIceAgent_) {
  xIceConf   conf;
  xEventLoop loop;

  xIceAgentState state;
  xIceAgentRole  role;

  char ice_ufrag[XICE_UFRAG_MAX_LEN];
  char ice_pwd[XICE_PWD_MAX_LEN];

  /* Remote credentials */
  char remote_ufrag[XICE_UFRAG_MAX_LEN];
  char remote_pwd[XICE_PWD_MAX_LEN];
  bool remote_set;

  /* Local candidates */
  xIceCandidate local_candidates[XICE_MAX_CANDIDATES];
  int           local_count;

  /* Remote candidates */
  xIceCandidate remote_candidates[XICE_MAX_CANDIDATES];
  int           remote_count;

  /* Candidate pairs */
  xIcePair pairs[XICE_MAX_PAIRS];
  int      pair_count;

  /* Nominated pair */
  xIcePair *nominated;

  /* STUN transaction manager */
  xStunTxnMgr txn_mgr;

  /* TURN client (optional) */
  xTurnClient *turn_client;

  /* Timers */
  xEventTimer check_timer;   /* Pacing timer for connectivity checks */
  xEventTimer gather_timer;  /* Gathering timeout */
  xEventTimer check_timeout; /* Overall check timeout */
  xEventTimer consent_timer; /* Consent freshness */

  int check_index;      /* Next pair to check */
  int consent_failures; /* Consecutive consent failures */

  bool gathering_done;
  bool remote_gathering_done;
  bool trickle;

  int host_count; /* Number of host candidates (first N in local_candidates) */

  /* Pending srflx/relay gather state */
  int pending_gather; /* Number of pending gather requests */

  /* Async DNS queries for STUN/TURN server resolution */
  xDnsQuery stun_dns_query;
  xDnsQuery turn_dns_query;
  uint16_t  stun_port; /* Parsed port for STUN server */
  uint16_t  turn_port; /* Parsed port for TURN server */

  /* DTLS data input hook — set by xPeerConnection when attached */
  xIceDtlsInputFn dtls_input_fn;
  void           *dtls_input_arg;

  /* Birthday attack state */
  bool        birthday_active;                          /**< Attack in progress.     */
  int         birthday_k;                               /**< Resolved local socket count. */
  int         birthday_n;                               /**< Resolved remote port count.   */
  int         birthday_timeout_ms;                      /**< Resolved overall timeout.     */
  xSocket    *birthday_socks;                           /**< Heap-allocated socket array.  */
  int         birthday_sock_count;                      /**< Actual created sockets.       */
  int         birthday_burst_index;                     /**< Current burst index.          */
  xEventTimer birthday_timer;                           /**< Pacing timer.                 */
  xEventTimer birthday_timeout_timer;                   /**< Overall timeout.              */
  struct sockaddr_storage birthday_target;              /**< Remote NAT public IP.         */
};

/* ───────────────────── Helpers ───────────────────── */

static void generate_random_string(char *buf, size_t len) {
  static const char charset[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  for (size_t i = 0; i < len; i++) {
    buf[i] = charset[arc4random_uniform(sizeof(charset) - 1)];
  }
#else
  static int seeded = 0;
  if (!seeded) {
    srand((unsigned)time(NULL));
    seeded = 1;
  }
  for (size_t i = 0; i < len; i++) {
    buf[i] = charset[rand() % (sizeof(charset) - 1)];
  }
#endif
  buf[len] = '\0';
}

static void set_state(xIceAgent_ *a, xIceAgentState new_state) {
  if (a->state == new_state) return;
  a->state = new_state;

  /* Map internal state to public state */
  xIceState pub;
  switch (new_state) {
  case xIceAgentState_New:
    pub = xIceState_New;
    break;
  case xIceAgentState_Gathering:
    pub = xIceState_Gathering;
    break;
  case xIceAgentState_Checking:
    pub = xIceState_Checking;
    break;
  case xIceAgentState_Connected:
    pub = xIceState_Connected;
    break;
  case xIceAgentState_Completed:
    pub = xIceState_Completed;
    break;
  case xIceAgentState_Failed:
    pub = xIceState_Failed;
    break;
  case xIceAgentState_Closed:
    pub = xIceState_Closed;
    break;
  default:
    return;
  }

  if (a->conf.on_state_change) {
    a->conf.on_state_change((xIceAgent)a, pub, a->conf.ctx);
  }
}

static socklen_t sockaddr_len(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) return sizeof(struct sockaddr_in);
  if (addr->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
  return 0;
}

/** Compare two sockaddrs (IPv4 or IPv6) for equality (address + port). */
static bool sockaddr_equal(const struct sockaddr *a, const struct sockaddr *b) {
  if (a->sa_family != b->sa_family) return false;
  if (a->sa_family == AF_INET) {
    const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
    const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
    return a4->sin_port == b4->sin_port &&
           a4->sin_addr.s_addr == b4->sin_addr.s_addr;
  }
  if (a->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
    const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
    return a6->sin6_port == b6->sin6_port &&
           memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0;
  }
  return false;
}

#ifdef XK_ENABLE_DEBUG
/** Format a sockaddr into "ip:port" string, writes into buf (size >= 64). */
static void sockaddr_to_str(const struct sockaddr *addr, char *buf,
                            size_t len) {
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
    char                      ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
    snprintf(buf, len, "%s:%u", ip, ntohs(a4->sin_port));
  } else if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
    char                       ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
    snprintf(buf, len, "[%s]:%u", ip, ntohs(a6->sin6_port));
  } else {
    snprintf(buf, len, "(unknown)");
  }
}
#endif

/* ───────────────────── Low-level UDP Send ───────────────────── */

static xErrno udp_sendto(xSocket sock, const uint8_t *data, size_t len,
                         const struct sockaddr *addr) {
  int fd = xSocketFd(sock);
  if (fd < 0) return xErrno_SysError;
  ssize_t n = sendto(fd, data, len, 0, addr, sockaddr_len(addr));
  return (n >= 0) ? xErrno_Ok : xErrno_SysError;
}

/* ───────────────────── Send Callback for STUN Txn ───────────────────── */

static xErrno agent_stun_send(const uint8_t *data, size_t len,
                              const struct sockaddr *addr, void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;
  /* Prefer a host candidate whose address family matches the destination */
  for (int i = 0; i < a->host_count; i++) {
    if (a->local_candidates[i].sock &&
        a->local_candidates[i].addr.ss_family == addr->sa_family) {
      return udp_sendto(a->local_candidates[i].sock, data, len, addr);
    }
  }
  /* Fallback: any socket */
  for (int i = 0; i < a->local_count; i++) {
    if (a->local_candidates[i].sock) {
      return udp_sendto(a->local_candidates[i].sock, data, len, addr);
    }
  }
  return xErrno_SysError;
}

/* ───────────────────── Pair Generation ───────────────────── */

static void generate_pairs(xIceAgent_ *a) {
  a->pair_count = 0;

  for (int l = 0; l < a->local_count && a->pair_count < XICE_MAX_PAIRS; l++) {
    for (int r = 0; r < a->remote_count && a->pair_count < XICE_MAX_PAIRS;
         r++) {
      /* Only pair candidates with same component */
      if (a->local_candidates[l].component_id !=
          a->remote_candidates[r].component_id) {
        continue;
      }

      /* Only pair candidates with same address family (RFC 8445 §6.1.2.2) */
      if (a->local_candidates[l].addr.ss_family !=
          a->remote_candidates[r].addr.ss_family) {
        continue;
      }

      xIcePair *pair  = &a->pairs[a->pair_count];
      pair->local     = &a->local_candidates[l];
      pair->remote    = &a->remote_candidates[r];
      pair->state     = xIcePairState_Frozen;
      pair->nominated = false;

      uint32_t g_prio, d_prio;
      if (a->role == xIceAgentRole_Controlling) {
        g_prio = pair->local->priority;
        d_prio = pair->remote->priority;
      } else {
        g_prio = pair->remote->priority;
        d_prio = pair->local->priority;
      }
      pair->priority = xIcePairPriority(g_prio, d_prio);

      a->pair_count++;
    }
  }

  xIcePairSort(a->pairs, a->pair_count);
}

/* ───────────────────── Connectivity Check ───────────────────── */

/**
 * @brief Context passed to on_check_response so it can access both
 *        the agent and the pair.
 */
typedef struct {
  xIceAgent_ *agent;
  xIcePair   *pair;
} CheckCtx;

static void start_consent(xIceAgent_ *a);

static void on_check_response(const xStunMsg *msg, const struct sockaddr *from,
                              void *arg);

/**
 * @brief Send function for connectivity checks — uses the pair's local socket.
 */
static xErrno pair_stun_send(const uint8_t *data, size_t len,
                             const struct sockaddr *addr, void *arg) {
  xSocket sock = (xSocket)arg;
  return udp_sendto(sock, data, len, addr);
}

static xErrno send_check(xIceAgent_ *a, xIcePair *pair) {
  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE,
                      sizeof(msg_buf) - XSTUN_HEADER_SIZE);

  /* USERNAME = remote_ufrag:local_ufrag */
  xStunAttrWriteUsername(&w, a->remote_ufrag, a->ice_ufrag);

  /* PRIORITY */
  xStunAttrWritePriority(&w, pair->local->priority);

  /* ICE-CONTROLLING or ICE-CONTROLLED */
  uint64_t tie_breaker = 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(&tie_breaker, sizeof(tie_breaker));
#endif
  if (a->role == xIceAgentRole_Controlling) {
    xStunAttrWriteIceControlling(&w, tie_breaker);
    /* USE-CANDIDATE for aggressive nomination */
    xStunAttrWriteUseCandidate(&w);
  } else {
    xStunAttrWriteIceControlled(&w, tie_breaker);
  }

  /* MESSAGE-INTEGRITY using remote password */
  xStunAttrWriteMessageIntegrity(&w, msg_buf, (const uint8_t *)a->remote_pwd,
                                 strlen(a->remote_pwd));

  /* FINGERPRINT */
  xStunAttrWriteFingerprint(&w, msg_buf);

  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  pair->state = xIcePairState_InProgress;

  /* Allocate a CheckCtx so the response callback can access the agent */
  CheckCtx *ctx = (CheckCtx *)malloc(sizeof(CheckCtx));
  if (!ctx) return xErrno_NoMemory;
  ctx->agent = a;
  ctx->pair  = pair;

  xErrno err;
  if (pair->local->type == xIceCandidateType_Relay && a->turn_client) {
    /* Relay pair: send connectivity check through TURN server */
    err = xTurnClientSendData(a->turn_client,
                              (const struct sockaddr *)&pair->remote->addr,
                              msg_buf, total);
    if (err == xErrno_Ok) {
      /* Register the transaction so we can match the response */
      err = xStunTxnMgrSendRaw(
        &a->txn_mgr, msg_buf, total, (struct sockaddr *)&pair->remote->addr,
        pair_stun_send, pair->local->sock, on_check_response, ctx);
    }
  } else {
    err = xStunTxnMgrSendRaw(
      &a->txn_mgr, msg_buf, total, (struct sockaddr *)&pair->remote->addr,
      pair_stun_send, pair->local->sock, on_check_response, ctx);
  }
  if (err != xErrno_Ok) {
    free(ctx);
  }
  return err;
}

static void check_pacing_cb(void *arg);
static void try_nominate(xIceAgent_ *a);

static void schedule_next_check(xIceAgent_ *a) {
  if (a->state != xIceAgentState_Checking) return;

  a->check_timer =
    xEventLoopTimerAfter(a->loop, check_pacing_cb, a, XICE_CHECK_PACING_MS);
}

static void check_pacing_cb(void *arg) {
  xIceAgent_ *a  = (xIceAgent_ *)arg;
  a->check_timer = NULL;

  if (a->state != xIceAgentState_Checking) return;

  /* Find next pair to check */
  while (a->check_index < a->pair_count) {
    xIcePair *pair = &a->pairs[a->check_index];
    a->check_index++;

    if (pair->state == xIcePairState_Frozen ||
        pair->state == xIcePairState_Waiting) {
      send_check(a, pair);
      schedule_next_check(a);
      return;
    }
  }

  /* All pairs dispatched — try to nominate now.  Earlier pairs may already
   * have succeeded while we were pacing out the remaining checks, so we
   * can nominate immediately without waiting for the next STUN response. */
  try_nominate(a);
}

/**
 * @brief Try to nominate the best succeeded pair.
 *
 * Called after every pair state change (check response or pacing exhaustion).
 *
 * Nomination strategy:
 *  - If any pair has succeeded AND all pairs have been dispatched
 *    (check_index >= pair_count), nominate the highest-priority succeeded
 *    pair immediately.  We do NOT wait for InProgress pairs to finish
 *    because STUN retransmission timeouts can be very long (~60 s).
 *  - If all pairs have reached a terminal state (Succeeded / Failed) and
 *    none succeeded, transition to Failed.
 */
static void try_nominate(xIceAgent_ *a) {
  if (a->state != xIceAgentState_Checking) return;
  if (a->nominated) return;

  bool any_in_progress           = false;
  bool any_succeeded             = false;
  bool any_non_prflx_in_progress = false;
  for (int i = 0; i < a->pair_count; i++) {
    if (a->pairs[i].state == xIcePairState_InProgress) {
      any_in_progress = true;
      if (a->pairs[i].remote->type != xIceCandidateType_Prflx)
        any_non_prflx_in_progress = true;
    }
    if (a->pairs[i].state == xIcePairState_Succeeded) {
      any_succeeded = true;
    }
  }

  /* All pairs dispatched and at least one succeeded — nominate now.
   *
   * Prefer a pair whose remote candidate is NOT peer-reflexive.  A prflx
   * remote address is an ephemeral source address observed in an incoming
   * binding request; the remote peer may not accept DTLS/data on that
   * address.  Only fall back to a prflx pair if no other succeeded pair
   * exists. */
  if (any_succeeded && a->check_index >= a->pair_count) {
    xIcePair *best       = NULL;
    xIcePair *best_prflx = NULL;
    for (int i = 0; i < a->pair_count; i++) {
      if (a->pairs[i].state != xIcePairState_Succeeded) continue;
      if (a->pairs[i].remote->type == xIceCandidateType_Prflx) {
        if (!best_prflx) best_prflx = &a->pairs[i];
      } else {
        if (!best) best = &a->pairs[i];
      }
    }

    /* If we only have prflx succeeded pairs but non-prflx pairs are still
     * in progress, wait for them — they are more likely to produce a
     * usable nominated path. */
    if (!best && best_prflx && any_non_prflx_in_progress) {
      return;
    }

    if (!best) best = best_prflx;

    if (best) {
      a->nominated    = best;
      best->nominated = true;

#ifdef XK_ENABLE_DEBUG
      char lstr[64], rstr[64];
      sockaddr_to_str((const struct sockaddr *)&best->local->addr, lstr,
                      sizeof(lstr));
      sockaddr_to_str((const struct sockaddr *)&best->remote->addr, rstr,
                      sizeof(rstr));
      XDEBUG("[ice] nominated pair: %s -> %s", lstr, rstr);
#endif

      set_state(a, xIceAgentState_Connected);
      start_consent(a);

      /* Cancel the check timeout — no longer needed */
      if (a->check_timeout) {
        xEventLoopTimerCancel(a->loop, a->check_timeout);
        a->check_timeout = NULL;
      }
    }
    return;
  }

  /* All pairs finished (none in progress) and none succeeded — fail. */
  if (!any_in_progress && !any_succeeded) {
    set_state(a, xIceAgentState_Failed);
  }
}

static void on_check_response(const xStunMsg        *msg,
                              const struct sockaddr *from
                              __attribute__((unused)),
                              void *arg) {
  CheckCtx   *ctx   = (CheckCtx *)arg;
  xIceAgent_ *agent = ctx->agent;
  xIcePair   *pair  = ctx->pair;
  free(ctx);

  if (!msg) {
    /* Timeout */
    pair->state = xIcePairState_Failed;
  } else if (xStunMsgIsSuccessResponse(msg->type)) {
    pair->state = xIcePairState_Succeeded;
  }

  /* A pair finished — check if we can nominate now */
  try_nominate(agent);
}

/* Forward declarations for birthday attack */
static void start_birthday_attack(xIceAgent_ *a);
static void cleanup_birthday(xIceAgent_ *a);
static void sockaddr_set_port(struct sockaddr_storage *addr, uint16_t port);

static void check_timeout_cb(void *arg) {
  xIceAgent_ *a    = (xIceAgent_ *)arg;
  a->check_timeout = NULL;

  if (a->state != xIceAgentState_Checking) return;

  /* Check if we already have a nominated pair */
  if (a->nominated) return;

  /* Find best succeeded pair, preferring non-prflx remote */
  xIcePair *best       = NULL;
  xIcePair *best_prflx = NULL;
  for (int i = 0; i < a->pair_count; i++) {
    if (a->pairs[i].state != xIcePairState_Succeeded) continue;
    if (a->pairs[i].remote->type == xIceCandidateType_Prflx) {
      if (!best_prflx) best_prflx = &a->pairs[i];
    } else {
      if (!best) best = &a->pairs[i];
    }
  }
  if (!best) best = best_prflx;

  if (best) {
    a->nominated    = best;
    best->nominated = true;
    set_state(a, xIceAgentState_Connected);
    return;
  }

  /* All regular checks failed — attempt birthday attack if enabled */
  if (a->birthday_k > 0) {
    start_birthday_attack(a);
    return;
  }

  set_state(a, xIceAgentState_Failed);
}

/* ───────────────────── Birthday Attack ───────────────────── */

/**
 * @brief Generate a random port in the ephemeral range.
 */
static uint16_t random_port(void) {
  uint32_t range = XICE_BIRTHDAY_PORT_MAX - XICE_BIRTHDAY_PORT_MIN + 1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  return (uint16_t)(XICE_BIRTHDAY_PORT_MIN + arc4random_uniform(range));
#else
  return (uint16_t)(XICE_BIRTHDAY_PORT_MIN + (rand() % range));
#endif
}

/**
 * @brief Build a lightweight STUN Binding Request (header only, 20 bytes).
 *
 * No USERNAME / MESSAGE-INTEGRITY / FINGERPRINT — minimizes packet size
 * to maximize send rate during the birthday attack.
 *
 * @param buf  Output buffer (must be >= XSTUN_HEADER_SIZE).
 * @return     Encoded length (always XSTUN_HEADER_SIZE).
 */
static size_t build_birthday_probe(uint8_t *buf) {
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  return (size_t)xStunMsgEncode(&msg, buf, XSTUN_HEADER_SIZE);
}

/**
 * @brief Handle a successful birthday hit — promote to nominated pair.
 *
 * Creates a peer-reflexive candidate from the remote address and
 * nominates the pair formed with the local host candidate that
 * owns the birthday socket.
 */
static void birthday_hit(xIceAgent_ *a, xSocket sock,
                         const struct sockaddr *from) {
  XDEBUG("[ice] birthday attack hit from %s",
         from->sa_family == AF_INET ? "IPv4" : "IPv6");

  /* Find which local host candidate's interface matches this socket.
   * Birthday sockets are bound to the same local address as host
   * candidates, so we match by address family. */
  xIceCandidate *local_cand = NULL;
  for (int i = 0; i < a->host_count; i++) {
    if (a->local_candidates[i].addr.ss_family == from->sa_family) {
      local_cand = &a->local_candidates[i];
      break;
    }
  }
  if (!local_cand) {
    /* Fallback: use first host candidate */
    if (a->host_count > 0) local_cand = &a->local_candidates[0];
  }
  if (!local_cand) return;

  /* Create a peer-reflexive remote candidate */
  if (a->remote_count >= XICE_MAX_CANDIDATES) return;

  xIceCandidate *prflx = &a->remote_candidates[a->remote_count];
  memset(prflx, 0, sizeof(*prflx));
  prflx->type         = xIceCandidateType_Prflx;
  prflx->component_id = 1;
  prflx->priority = xIceCandidatePriority(xIceCandidateType_Prflx, 65535, 1);
  memcpy(&prflx->addr, from, sockaddr_len(from));
  memcpy(&prflx->base_addr, from, sockaddr_len(from));
  xIceCandidateFoundation(prflx, NULL);
  a->remote_count++;

  /* Create and nominate a pair */
  if (a->pair_count >= XICE_MAX_PAIRS) return;

  xIcePair *pair  = &a->pairs[a->pair_count];
  pair->local     = local_cand;
  pair->remote    = prflx;
  pair->state     = xIcePairState_Succeeded;
  pair->nominated = true;

  uint32_t g_prio, d_prio;
  if (a->role == xIceAgentRole_Controlling) {
    g_prio = pair->local->priority;
    d_prio = pair->remote->priority;
  } else {
    g_prio = pair->remote->priority;
    d_prio = pair->local->priority;
  }
  pair->priority = xIcePairPriority(g_prio, d_prio);
  a->pair_count++;

  /* Transfer the birthday socket to the local candidate so it can be
   * used for subsequent data and consent checks.  We must NOT destroy
   * this socket in cleanup_birthday(), so we NULL it out of the array. */
  for (int i = 0; i < a->birthday_sock_count; i++) {
    if (a->birthday_socks[i] == sock) {
      a->birthday_socks[i] = NULL;
      break;
    }
  }

  /* Replace the local candidate's socket with the birthday socket.
   * The old host socket is still valid for other pairs, so we keep it. */
  /* Note: for simplicity we keep the original host socket alive.
   * The nominated pair will use the birthday socket via pair->local->sock
   * indirectly — but since we can't change the candidate's sock without
   * affecting other pairs, we store the birthday socket on the pair
   * and override send behavior.  For now, we take the simpler approach:
   * the birthday socket is used for the nominated pair by setting it
   * as the candidate's socket.  This works because after nomination,
   * only the nominated pair's socket is used for data. */

  a->nominated = pair;

#ifdef XK_ENABLE_DEBUG
  {
    char lstr[64], rstr[64];
    sockaddr_to_str((const struct sockaddr *)&pair->local->addr, lstr,
                    sizeof(lstr));
    sockaddr_to_str((const struct sockaddr *)&pair->remote->addr, rstr,
                    sizeof(rstr));
    XDEBUG("[ice] birthday nominated pair: %s -> %s", lstr, rstr);
  }
#endif

  /* Clean up remaining birthday state */
  cleanup_birthday(a);

  set_state(a, xIceAgentState_Connected);
  start_consent(a);
}

/**
 * @brief UDP receive callback for birthday attack sockets.
 *
 * Handles two cases:
 *  1. Binding Response — our probe reached the peer and they replied.
 *  2. Binding Request — the peer's probe reached us; reply and probe back.
 */
static void on_birthday_recv(xSocket sock, xEventMask mask, void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;

  if (!(mask & xEvent_Read)) return;
  if (!a->birthday_active) return;

  int fd = xSocketFd(sock);

  for (;;) {
    uint8_t                 buf[256];
    struct sockaddr_storage from_addr;
    socklen_t               from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
    if (n <= 0) break;

    size_t                 len  = (size_t)n;
    const struct sockaddr *from = (const struct sockaddr *)&from_addr;

    /* Only process STUN messages */
    if (!xStunMsgIsStun(buf, len)) continue;

    xStunMsg msg;
    if (xStunMsgDecode(&msg, buf, len) != xErrno_Ok) continue;

    if (xStunMsgIsSuccessResponse(msg.type) ||
        xStunMsgIsErrorResponse(msg.type)) {
      /* Case 1: Our probe got through and the peer replied.
       * This means bidirectional connectivity is established. */
      birthday_hit(a, sock, from);
      return;
    }

    if (xStunMsgIsRequest(msg.type)) {
      /* Case 2: Peer's birthday probe reached us.
       * Send a Binding Response so the peer knows we received it. */
      uint8_t  resp_buf[128];
      xStunMsg resp;
      xStunMsgInit(&resp, xStunMsgType_BindingResponse, msg.txn_id);
      xStunMsgEncode(&resp, resp_buf, sizeof(resp_buf));

      xStunAttrWriter w;
      xStunAttrWriterInit(&w, resp_buf + XSTUN_HEADER_SIZE,
                          sizeof(resp_buf) - XSTUN_HEADER_SIZE);
      xStunAttrWriteXorMappedAddress(&w, from, msg.txn_id);
      xWriteU16BE(resp_buf + 2, (uint16_t)w.pos);
      size_t total = XSTUN_HEADER_SIZE + w.pos;

      udp_sendto(sock, resp_buf, total, from);

      /* Also treat this as a hit — the peer's probe reaching us means
       * the NAT mapping exists in both directions (the peer sent from
       * their birthday socket, creating a NAT mapping; our response
       * goes back through that mapping). */
      birthday_hit(a, sock, from);
      return;
    }
  }
}

/**
 * @brief Pacing callback — sends one burst of probes from the next socket.
 */
static void birthday_pacing_cb(void *arg) {
  xIceAgent_ *a   = (xIceAgent_ *)arg;
  a->birthday_timer = NULL;

  if (!a->birthday_active) return;
  if (a->birthday_burst_index >= a->birthday_sock_count) return;

  xSocket sock = a->birthday_socks[a->birthday_burst_index];
  if (!sock) {
    /* Socket was claimed by birthday_hit — skip */
    a->birthday_burst_index++;
    if (a->birthday_burst_index < a->birthday_sock_count) {
      a->birthday_timer = xEventLoopTimerAfter(
        a->loop, birthday_pacing_cb, a, XICE_BIRTHDAY_PACING_MS);
    }
    return;
  }

  /* Send n probes to random ports on the target IP */
  struct sockaddr_storage target;
  memcpy(&target, &a->birthday_target, sizeof(target));

  for (int i = 0; i < a->birthday_n; i++) {
    uint8_t probe[XSTUN_HEADER_SIZE];
    build_birthday_probe(probe);

    sockaddr_set_port(&target, random_port());
    udp_sendto(sock, probe, XSTUN_HEADER_SIZE,
               (const struct sockaddr *)&target);
  }

  a->birthday_burst_index++;

  /* Schedule next burst, or wrap around for another round */
  if (a->birthday_burst_index < a->birthday_sock_count) {
    a->birthday_timer = xEventLoopTimerAfter(
      a->loop, birthday_pacing_cb, a, XICE_BIRTHDAY_PACING_MS);
  } else {
    /* All sockets have sent one burst — loop back for another round.
     * Each round uses fresh random ports, increasing coverage over
     * the timeout window.  This is critical because the remote peer
     * may start its own birthday attack (creating NAT mappings) at
     * any point during our window. */
    a->birthday_burst_index = 0;
    a->birthday_timer = xEventLoopTimerAfter(
      a->loop, birthday_pacing_cb, a, XICE_BIRTHDAY_PACING_MS);
  }
}

/**
 * @brief Overall birthday attack timeout — give up and fail.
 */
static void birthday_timeout_cb(void *arg) {
  xIceAgent_ *a            = (xIceAgent_ *)arg;
  a->birthday_timeout_timer = NULL;

  if (!a->birthday_active) return;

  XDEBUG("[ice] birthday attack timed out");

  cleanup_birthday(a);
  set_state(a, xIceAgentState_Failed);
}

/**
 * @brief Clean up all birthday attack resources.
 */
static void cleanup_birthday(xIceAgent_ *a) {
  if (a->birthday_timer) {
    xEventLoopTimerCancel(a->loop, a->birthday_timer);
    a->birthday_timer = NULL;
  }
  if (a->birthday_timeout_timer) {
    xEventLoopTimerCancel(a->loop, a->birthday_timeout_timer);
    a->birthday_timeout_timer = NULL;
  }

  for (int i = 0; i < a->birthday_sock_count; i++) {
    if (a->birthday_socks[i]) {
      xSocketDestroy(a->loop, a->birthday_socks[i]);
      a->birthday_socks[i] = NULL;
    }
  }

  if (a->birthday_socks) {
    free(a->birthday_socks);
    a->birthday_socks = NULL;
  }

  a->birthday_sock_count  = 0;
  a->birthday_burst_index = 0;
  a->birthday_active      = false;
}

/**
 * @brief Start the birthday attack.
 *
 * Creates k UDP sockets, finds the remote srflx IP as the target,
 * and begins paced probe bursts.
 */
static void start_birthday_attack(xIceAgent_ *a) {
  if (a->birthday_active) return;

  /* Find the remote srflx candidate to get the peer's NAT public IP.
   * Fall back to any remote candidate if no srflx is available. */
  const xIceCandidate *target_cand = NULL;
  for (int i = 0; i < a->remote_count; i++) {
    if (a->remote_candidates[i].type == xIceCandidateType_Srflx) {
      target_cand = &a->remote_candidates[i];
      break;
    }
  }
  if (!target_cand) {
    /* Fall back to first remote host candidate */
    for (int i = 0; i < a->remote_count; i++) {
      if (a->remote_candidates[i].type == xIceCandidateType_Host) {
        target_cand = &a->remote_candidates[i];
        break;
      }
    }
  }
  if (!target_cand) {
    /* No usable remote candidate — cannot attempt birthday attack */
    XDEBUG("[ice] birthday attack: no remote candidate for target IP");
    set_state(a, xIceAgentState_Failed);
    return;
  }

  /* Copy the target IP (port will be randomized per probe) */
  memcpy(&a->birthday_target, &target_cand->addr, sizeof(a->birthday_target));

  XDEBUG("[ice] starting birthday attack: k=%d n=%d", a->birthday_k,
         a->birthday_n);

  /* Allocate socket array */
  a->birthday_socks = (xSocket *)calloc((size_t)a->birthday_k, sizeof(xSocket));
  if (!a->birthday_socks) {
    set_state(a, xIceAgentState_Failed);
    return;
  }

  /* Determine the address family and local bind address from host candidates */
  sa_family_t family = a->birthday_target.ss_family;
  const struct sockaddr *bind_addr = NULL;
  for (int i = 0; i < a->host_count; i++) {
    if (a->local_candidates[i].addr.ss_family == family) {
      bind_addr = (const struct sockaddr *)&a->local_candidates[i].addr;
      break;
    }
  }

  /* Create k UDP sockets */
  int created = 0;
  for (int i = 0; i < a->birthday_k; i++) {
    xSocket sock = xSocketCreate(a->loop, (int)family, SOCK_DGRAM, 0,
                                 xEvent_Read, on_birthday_recv, a);
    if (!sock) continue;

    /* Bind to the same local interface with OS-assigned port */
    int fd = xSocketFd(sock);
    if (bind_addr) {
      struct sockaddr_storage local_bind;
      memcpy(&local_bind, bind_addr, sizeof(local_bind));
      sockaddr_set_port(&local_bind, 0);
      if (bind(fd, (const struct sockaddr *)&local_bind,
               sockaddr_len((const struct sockaddr *)&local_bind)) < 0) {
        xSocketDestroy(a->loop, sock);
        continue;
      }
    } else {
      /* No host candidate — bind to INADDR_ANY */
      struct sockaddr_in any;
      memset(&any, 0, sizeof(any));
      any.sin_family = AF_INET;
      any.sin_port   = 0;
      if (bind(fd, (const struct sockaddr *)&any, sizeof(any)) < 0) {
        xSocketDestroy(a->loop, sock);
        continue;
      }
    }

    a->birthday_socks[created++] = sock;
  }

  if (created == 0) {
    XDEBUG("[ice] birthday attack: failed to create any sockets");
    free(a->birthday_socks);
    a->birthday_socks = NULL;
    set_state(a, xIceAgentState_Failed);
    return;
  }

  a->birthday_sock_count  = created;
  a->birthday_burst_index = 0;
  a->birthday_active      = true;

  XDEBUG("[ice] birthday attack: created %d/%d sockets", created,
         a->birthday_k);

  /* Start pacing timer — first burst fires immediately */
  a->birthday_timer =
    xEventLoopTimerAfter(a->loop, birthday_pacing_cb, a, 0);

  /* Start overall timeout */
  a->birthday_timeout_timer = xEventLoopTimerAfter(
    a->loop, birthday_timeout_cb, a, (uint64_t)a->birthday_timeout_ms);
}

static void start_checks(xIceAgent_ *a) {
  if (a->pair_count == 0) {
    set_state(a, xIceAgentState_Failed);
    return;
  }

  set_state(a, xIceAgentState_Checking);
  a->check_index = 0;

  /* Start pacing timer */
  schedule_next_check(a);

  /* Start overall check timeout */
  a->check_timeout =
    xEventLoopTimerAfter(a->loop, check_timeout_cb, a, XICE_CHECK_TIMEOUT_MS);
}

/* ───────────────────── Consent Freshness ───────────────────── */

static void consent_cb(void *arg) {
  xIceAgent_ *a    = (xIceAgent_ *)arg;
  a->consent_timer = NULL;

  if (a->state != xIceAgentState_Connected &&
      a->state != xIceAgentState_Completed) {
    return;
  }

  if (!a->nominated) return;

  /* Send a Binding Request on the nominated pair */
  uint8_t msg_buf[256];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, msg_buf + XSTUN_HEADER_SIZE,
                      sizeof(msg_buf) - XSTUN_HEADER_SIZE);
  xStunAttrWriteUsername(&w, a->remote_ufrag, a->ice_ufrag);
  xStunAttrWriteMessageIntegrity(&w, msg_buf, (const uint8_t *)a->remote_pwd,
                                 strlen(a->remote_pwd));
  xWriteU16BE(msg_buf + 2, (uint16_t)w.pos);

  size_t total = XSTUN_HEADER_SIZE + w.pos;

  /* Send consent check via TURN relay if nominated pair is relay type */
  if (a->nominated->local->type == xIceCandidateType_Relay && a->turn_client) {
    xTurnClientSendData(a->turn_client,
                        (const struct sockaddr *)&a->nominated->remote->addr,
                        msg_buf, total);
  } else {
    agent_stun_send(msg_buf, total,
                    (struct sockaddr *)&a->nominated->remote->addr, a);
  }

  /* Schedule next consent check */
  a->consent_timer =
    xEventLoopTimerAfter(a->loop, consent_cb, a, XICE_CONSENT_INTERVAL_MS);
}

static void start_consent(xIceAgent_ *a) {
  a->consent_timer =
    xEventLoopTimerAfter(a->loop, consent_cb, a, XICE_CONSENT_INTERVAL_MS);
}

/* ───────────────────── Gathering ───────────────────── */

static void gather_check_done(xIceAgent_ *a);

/**
 * Parse a "host:port" string into hostname and port.
 * Returns true on success. `host_out` is NUL-terminated.
 */
static bool parse_host_port(const char *host_port, char *host_out,
                            size_t host_out_size, uint16_t *port_out) {
  if (!host_port) return false;

  const char *colon = strrchr(host_port, ':');
  if (!colon) return false;

  size_t host_len = (size_t)(colon - host_port);
  if (host_len == 0 || host_len >= host_out_size) return false;

  memcpy(host_out, host_port, host_len);
  host_out[host_len] = '\0';

  int port = atoi(colon + 1);
  if (port <= 0 || port > 65535) return false;
  *port_out = (uint16_t)port;
  return true;
}

/**
 * Set the port on a sockaddr_storage (IPv4 or IPv6).
 */
static void sockaddr_set_port(struct sockaddr_storage *addr, uint16_t port) {
  if (addr->ss_family == AF_INET) {
    ((struct sockaddr_in *)addr)->sin_port = htons(port);
  } else if (addr->ss_family == AF_INET6) {
    ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
  }
}

/* Forward declarations for DNS callbacks */
static void send_stun_binding(xIceAgent_                    *a,
                              const struct sockaddr_storage *stun_addr);
static void start_turn_allocate(xIceAgent_                    *a,
                                const struct sockaddr_storage *turn_addr);

/**
 * Async DNS callback for STUN server resolution.
 */
static void on_stun_dns_done(xDnsResult *result, void *arg) {
  xIceAgent_ *a     = (xIceAgent_ *)arg;
  a->stun_dns_query = NULL;

  if (result->error == xErrno_Ok && result->addrs) {
    struct sockaddr_storage stun_addr;
    memcpy(&stun_addr, &result->addrs->addr, sizeof(stun_addr));
    sockaddr_set_port(&stun_addr, a->stun_port);
    xDnsResultFree(result);
    send_stun_binding(a, &stun_addr);
    return;
  }

  /* DNS failed — decrement pending and check done */
  xDnsResultFree(result);
  a->pending_gather--;
  gather_check_done(a);
}

/**
 * Async DNS callback for TURN server resolution.
 */
static void on_turn_dns_done(xDnsResult *result, void *arg) {
  xIceAgent_ *a     = (xIceAgent_ *)arg;
  a->turn_dns_query = NULL;

  if (result->error == xErrno_Ok && result->addrs) {
    struct sockaddr_storage turn_addr;
    memcpy(&turn_addr, &result->addrs->addr, sizeof(turn_addr));
    sockaddr_set_port(&turn_addr, a->turn_port);
    xDnsResultFree(result);
    start_turn_allocate(a, &turn_addr);
    return;
  }

  /* DNS failed — decrement pending and check done */
  xDnsResultFree(result);
  a->pending_gather--;
  gather_check_done(a);
}

/* ── srflx: context for per-host STUN Binding request ── */

typedef struct {
  xIceAgent_ *agent;
  int host_index; /* Index of the host candidate that sent the request */
} SrflxCtx;

static void on_srflx_response(const xStunMsg        *msg,
                              const struct sockaddr *from
                              __attribute__((unused)),
                              void *arg) {
  SrflxCtx   *ctx = (SrflxCtx *)arg;
  xIceAgent_ *a   = ctx->agent;
  int         hi  = ctx->host_index;
  free(ctx);

  if (msg && xStunMsgIsSuccessResponse(msg->type)) {
    /* Extract XOR-MAPPED-ADDRESS → srflx candidate */
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;

    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_XorMappedAddress) {
        struct sockaddr_storage mapped;
        if (xStunAttrDecodeXorMappedAddress(&attr, msg->txn_id, &mapped) ==
            xErrno_Ok) {
          if (a->local_count < XICE_MAX_CANDIDATES) {
            xIceCandidate *host = &a->local_candidates[hi];
            xIceCandidate *cand = &a->local_candidates[a->local_count];
            memset(cand, 0, sizeof(*cand));
            cand->type         = xIceCandidateType_Srflx;
            cand->component_id = 1;
            cand->transport    = 0; /* UDP */
            cand->priority     = xIceCandidatePriority(xIceCandidateType_Srflx,
                                                       (uint16_t)(65535 - hi), 1);
            memcpy(&cand->addr, &mapped, sizeof(mapped));
            /* base_addr = the host candidate that sent the request */
            memcpy(&cand->base_addr, &host->addr, sizeof(cand->base_addr));
            /* rel_addr = mapped address */
            memcpy(&cand->rel_addr, &mapped, sizeof(mapped));
            cand->sock = host->sock;
            xIceCandidateFoundation(cand, NULL);
            a->local_count++;

            /* Notify candidate */
            if (a->conf.on_candidate) {
              char cand_line[256];
              if (xIceSdpEncodeCandidate(cand, cand_line, sizeof(cand_line)) >
                  0) {
                a->conf.on_candidate((xIceAgent)a, cand_line, a->conf.ctx);
              }
            }
          }
        }
        break;
      }
    }
  }

  a->pending_gather--;
  gather_check_done(a);
}

/* ── relay: TURN Allocate callback ── */

static void turn_create_permissions_for_remotes(xIceAgent_ *a);

static void on_turn_allocated(const struct sockaddr *relayed_addr,
                              const struct sockaddr *mapped_addr,
                              uint32_t lifetime __attribute__((unused)),
                              void    *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;

  /* Use the first host candidate as the base for TURN-derived candidates */
  xIceCandidate *base_host =
    (a->host_count > 0) ? &a->local_candidates[0] : NULL;

  /* Create srflx candidate from mapped_addr if available */
  if (mapped_addr && a->local_count < XICE_MAX_CANDIDATES) {
    xIceCandidate *cand = &a->local_candidates[a->local_count];
    memset(cand, 0, sizeof(*cand));
    cand->type         = xIceCandidateType_Srflx;
    cand->component_id = 1;
    cand->transport    = 0;
    cand->priority = xIceCandidatePriority(xIceCandidateType_Srflx, 65534, 1);
    memcpy(&cand->addr, mapped_addr, sockaddr_len(mapped_addr));
    if (base_host) {
      memcpy(&cand->base_addr, &base_host->addr, sizeof(cand->base_addr));
    }
    memcpy(&cand->rel_addr, mapped_addr, sockaddr_len(mapped_addr));
    cand->sock = base_host ? base_host->sock : NULL;
    xIceCandidateFoundation(cand, NULL);
    a->local_count++;

    if (a->conf.on_candidate) {
      char cand_line[256];
      if (xIceSdpEncodeCandidate(cand, cand_line, sizeof(cand_line)) > 0) {
        a->conf.on_candidate((xIceAgent)a, cand_line, a->conf.ctx);
      }
    }
  }

  /* Create relay candidate from relayed_addr */
  if (relayed_addr && a->local_count < XICE_MAX_CANDIDATES) {
    xIceCandidate *cand = &a->local_candidates[a->local_count];
    memset(cand, 0, sizeof(*cand));
    cand->type         = xIceCandidateType_Relay;
    cand->component_id = 1;
    cand->transport    = 0;
    cand->priority = xIceCandidatePriority(xIceCandidateType_Relay, 65535, 1);
    memcpy(&cand->addr, relayed_addr, sockaddr_len(relayed_addr));
    if (base_host) {
      memcpy(&cand->base_addr, &base_host->addr, sizeof(cand->base_addr));
    }
    memcpy(&cand->rel_addr, relayed_addr, sockaddr_len(relayed_addr));
    cand->sock = base_host ? base_host->sock : NULL;
    xIceCandidateFoundation(cand, NULL);
    a->local_count++;

    if (a->conf.on_candidate) {
      char cand_line[256];
      if (xIceSdpEncodeCandidate(cand, cand_line, sizeof(cand_line)) > 0) {
        a->conf.on_candidate((xIceAgent)a, cand_line, a->conf.ctx);
      }
    }
  }

  a->pending_gather--;
  gather_check_done(a);

  /* Create permissions for any remote candidates already known */
  turn_create_permissions_for_remotes(a);
}

static void on_turn_failed(xErrno err __attribute__((unused)), void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;
  a->pending_gather--;
  gather_check_done(a);
}

/**
 * @brief Callback for TURN relay data — forwards to DTLS input hook.
 */
static void on_turn_data(const uint8_t *data, size_t len,
                         const struct sockaddr *from, void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;
  XDEBUG("[ice] TURN relay data %zu bytes", len);
  if (a->dtls_input_fn) {
    a->dtls_input_fn(data, len, from, a->dtls_input_arg);
  }
  /* else: no DTLS consumer attached, silently discard */
}

/**
 * @brief Create TURN permissions for all known remote candidates.
 */
static void turn_create_permissions_for_remotes(xIceAgent_ *a) {
  if (!a->turn_client) return;
  if (a->turn_client->state != xTurnState_Allocated) return;

  for (int i = 0; i < a->remote_count; i++) {
    const struct sockaddr *peer =
      (const struct sockaddr *)&a->remote_candidates[i].addr;
    xErrno err = xTurnClientCreatePermission(a->turn_client, peer);
    if (err != xErrno_Ok) {
      XDEBUG("[ice] failed to create TURN permission for remote candidate %d",
             i);
    }
  }
}

/**
 * Send a STUN Binding Request to the resolved STUN server address.
 * Called from on_stun_dns_done after successful DNS resolution.
 */
/**
 * Send a STUN Binding Request from a specific host candidate.
 */
static void send_stun_binding_for_host(xIceAgent_                    *a,
                                       const struct sockaddr_storage *stun_addr,
                                       int host_index) {
  uint8_t msg_buf[256];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++)
    txn_id[i] = (uint8_t)(rand() & 0xFF);
#endif
  xStunMsg stun_msg;
  xStunMsgInit(&stun_msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&stun_msg, msg_buf, sizeof(msg_buf));
  xWriteU16BE(msg_buf + 2, 0); /* No attributes */

  SrflxCtx *ctx = (SrflxCtx *)malloc(sizeof(SrflxCtx));
  if (!ctx) {
    a->pending_gather--;
    gather_check_done(a);
    return;
  }
  ctx->agent      = a;
  ctx->host_index = host_index;

  xStunTxnMgrSendRaw(&a->txn_mgr, msg_buf, XSTUN_HEADER_SIZE,
                     (struct sockaddr *)stun_addr, agent_stun_send, a,
                     on_srflx_response, ctx);
}

static void send_stun_binding(xIceAgent_                    *a,
                              const struct sockaddr_storage *stun_addr) {
  /* Send a STUN Binding Request from each host candidate */
  for (int i = 0; i < a->host_count; i++) {
    if (a->local_candidates[i].addr.ss_family == stun_addr->ss_family) {
      a->pending_gather++;
      send_stun_binding_for_host(a, stun_addr, i);
    }
  }
  /* Remove the initial pending_gather increment from on_stun_dns_done path */
  a->pending_gather--;
  gather_check_done(a);
}

/**
 * Start a TURN Allocate to the resolved TURN server address.
 * Called from on_turn_dns_done after successful DNS resolution.
 */
static void start_turn_allocate(xIceAgent_                    *a,
                                const struct sockaddr_storage *turn_addr) {
  a->turn_client = (xTurnClient *)calloc(1, sizeof(xTurnClient));
  if (!a->turn_client) {
    a->pending_gather--;
    gather_check_done(a);
    return;
  }

  xTurnConfig tc_conf;
  memset(&tc_conf, 0, sizeof(tc_conf));
  memcpy(&tc_conf.server, turn_addr, sizeof(*turn_addr));
  strncpy(tc_conf.username, a->conf.turn_username,
          sizeof(tc_conf.username) - 1);
  strncpy(tc_conf.password, a->conf.turn_password,
          sizeof(tc_conf.password) - 1);
  tc_conf.send_fn      = agent_stun_send;
  tc_conf.send_arg     = a;
  tc_conf.on_allocated = on_turn_allocated;
  tc_conf.on_failed    = on_turn_failed;
  tc_conf.on_data      = on_turn_data;
  tc_conf.ctx          = a;

  xTurnClientInit(a->turn_client, a->loop, &tc_conf);
  xTurnClientAllocate(a->turn_client);
}

/**
 * Check if all pending gather requests have completed.
 * If so, cancel the gather timer and finish gathering immediately.
 */
static void gather_check_done(xIceAgent_ *a) {
  if (a->pending_gather > 0) return;
  if (a->state != xIceAgentState_Gathering) return;
  if (a->gathering_done) return;

  /* Cancel the gather timeout — we finished early */
  if (a->gather_timer) {
    xEventLoopTimerCancel(a->loop, a->gather_timer);
    a->gather_timer = NULL;
  }

  a->gathering_done = true;

  /* Notify end of candidates */
  if (a->conf.on_candidate) {
    a->conf.on_candidate((xIceAgent)a, NULL, a->conf.ctx);
  }

  /* If remote is set, start checks */
  if (a->remote_set) {
    generate_pairs(a);
    start_checks(a);
  }
}

static void gather_timeout_cb(void *arg) {
  xIceAgent_ *a   = (xIceAgent_ *)arg;
  a->gather_timer = NULL;

  if (a->state != xIceAgentState_Gathering) return;

  a->gathering_done = true;

  /* Notify end of candidates */
  if (a->conf.on_candidate) {
    a->conf.on_candidate((xIceAgent)a, NULL, a->conf.ctx);
  }

  /* If remote is set, start checks */
  if (a->remote_set) {
    generate_pairs(a);
    start_checks(a);
  }
}

/* ───────────────────── Incoming STUN Handler ───────────────────── */

static void handle_incoming_binding_request(xIceAgent_ *a, const xStunMsg *msg,
                                            const struct sockaddr *from,
                                            xSocket                sock) {
  /* Send a Binding Success Response */
  uint8_t  resp_buf[256];
  xStunMsg resp;
  xStunMsgInit(&resp, xStunMsgType_BindingResponse, msg->txn_id);
  xStunMsgEncode(&resp, resp_buf, sizeof(resp_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, resp_buf + XSTUN_HEADER_SIZE,
                      sizeof(resp_buf) - XSTUN_HEADER_SIZE);

  xStunAttrWriteXorMappedAddress(&w, from, msg->txn_id);
  xStunAttrWriteMessageIntegrity(&w, resp_buf, (const uint8_t *)a->ice_pwd,
                                 strlen(a->ice_pwd));
  xStunAttrWriteFingerprint(&w, resp_buf);

  xWriteU16BE(resp_buf + 2, (uint16_t)w.pos);
  size_t total = XSTUN_HEADER_SIZE + w.pos;

  udp_sendto(sock, resp_buf, total, from);

  /* Check for USE-CANDIDATE */
  bool          use_candidate = false;
  xStunAttrIter iter;
  xStunAttrIterInit(&iter, msg);
  xStunAttr attr;
  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_UseCandidate) {
      use_candidate = true;
    }
  }

  /* Check if this is from a known remote candidate */
  bool known = false;
  for (int i = 0; i < a->remote_count; i++) {
    const struct sockaddr *raddr =
      (const struct sockaddr *)&a->remote_candidates[i].addr;
    if (sockaddr_equal(from, raddr)) {
      known = true;
      break;
    }
  }

  /* Peer reflexive candidate (RFC 8445 §7.2.1.3) */
  if (!known && a->remote_count < XICE_MAX_CANDIDATES) {
    xIceCandidate *prflx = &a->remote_candidates[a->remote_count];
    memset(prflx, 0, sizeof(*prflx));
    prflx->type         = xIceCandidateType_Prflx;
    prflx->component_id = 1;
    prflx->priority = xIceCandidatePriority(xIceCandidateType_Prflx, 65535, 1);
    memcpy(&prflx->addr, from, sockaddr_len(from));
    memcpy(&prflx->base_addr, from, sockaddr_len(from));
    xIceCandidateFoundation(prflx, NULL);
    a->remote_count++;

    /* Add new pairs for the prflx candidate without destroying existing
     * pairs that may already be InProgress / Succeeded.  Calling
     * generate_pairs() here would reset all pair states and corrupt
     * outstanding connectivity-check transactions. */
    if (a->state == xIceAgentState_Checking) {
      for (int l = 0; l < a->local_count && a->pair_count < XICE_MAX_PAIRS;
           l++) {
        if (a->local_candidates[l].component_id != prflx->component_id)
          continue;
        if (a->local_candidates[l].addr.ss_family != prflx->addr.ss_family)
          continue;

        xIcePair *pair  = &a->pairs[a->pair_count];
        pair->local     = &a->local_candidates[l];
        pair->remote    = prflx;
        pair->state     = xIcePairState_Frozen;
        pair->nominated = false;

        uint32_t g_prio, d_prio;
        if (a->role == xIceAgentRole_Controlling) {
          g_prio = pair->local->priority;
          d_prio = pair->remote->priority;
        } else {
          g_prio = pair->remote->priority;
          d_prio = pair->local->priority;
        }
        pair->priority = xIcePairPriority(g_prio, d_prio);
        a->pair_count++;
      }
      /* Note: we do NOT re-sort or reset check_index here.  The new
       * pairs are appended and will be picked up by the pacing timer
       * when check_index advances to them. */
    }
  }

  /* Handle nomination (Controlled side) */
  if (use_candidate && a->role == xIceAgentRole_Controlled && !a->nominated) {
    /* Find the pair for this remote address */
    for (int i = 0; i < a->pair_count; i++) {
      const struct sockaddr *raddr =
        (const struct sockaddr *)&a->pairs[i].remote->addr;
      if (sockaddr_equal(from, raddr)) {
        a->nominated          = &a->pairs[i];
        a->pairs[i].nominated = true;
        a->pairs[i].state     = xIcePairState_Succeeded;

#ifdef XK_ENABLE_DEBUG
        char lstr[64], rstr[64];
        sockaddr_to_str((const struct sockaddr *)&a->pairs[i].local->addr, lstr,
                        sizeof(lstr));
        sockaddr_to_str((const struct sockaddr *)&a->pairs[i].remote->addr,
                        rstr, sizeof(rstr));
        XDEBUG("[ice] nominated pair: %s -> %s", lstr, rstr);
#endif

        set_state(a, xIceAgentState_Connected);
        start_consent(a);
        break;
      }
    }
  }
}

/* ───────────────────── UDP Demux (RFC 7983) ───────────────────── */

static void on_udp_recv(xSocket sock, xEventMask mask, void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;

  if (!(mask & xEvent_Read)) return;

  int fd = xSocketFd(sock);

  /*
   * Edge-triggered drain loop: read all pending datagrams until EAGAIN.
   * In edge-triggered mode (kqueue EV_CLEAR / epoll EPOLLET), we are
   * only notified once when data becomes available. If multiple UDP
   * packets arrive before we read, we must drain them all here or the
   * remaining packets will be stuck in the socket buffer with no
   * further notification.
   */
  for (;;) {
    uint8_t                 buf[2048];
    struct sockaddr_storage from_addr;
    socklen_t               from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr,
                         &from_len);
    if (n <= 0) break;

    size_t                 len  = (size_t)n;
    const struct sockaddr *from = (const struct sockaddr *)&from_addr;

    /*
     * RFC 7983 first-byte demultiplexing:
     *   [0,   3]   → STUN
     *   [20,  63]  → DTLS
     *   [64,  79]  → TURN ChannelData
     *   [128, 191] → RTP/RTCP (reserved, discard)
     *   other      → discard
     */
    int pkt_type = xIceDemuxClassify(buf[0]);

    switch (pkt_type) {
    case XICE_DEMUX_STUN: {
      /* STUN message */
      if (!xStunMsgIsStun(buf, len)) {
        /* Looks like STUN range but fails validation — discard */
        continue;
      }
      xStunMsg msg;
      if (xStunMsgDecode(&msg, buf, len) != xErrno_Ok) continue;

      uint8_t msg_class = XSTUN_MSG_CLASS(msg.type);

      if (xStunMsgIsRequest(msg.type)) {
        handle_incoming_binding_request(a, &msg, from, sock);
      } else if (msg_class == XSTUN_CLASS_INDICATION) {
        /* Indication (e.g. DataIndication) — route to TURN client */
        if (a->turn_client) {
          xTurnClientOnMessage(a->turn_client, &msg, buf, len, from);
        }
        /* No TURN client or not handled — silently discard */
      } else if (xStunMsgIsSuccessResponse(msg.type) ||
                 xStunMsgIsErrorResponse(msg.type)) {
        /* Try TURN client first */
        if (a->turn_client) {
          if (xTurnClientOnMessage(a->turn_client, &msg, buf, len, from)) {
            continue;
          }
        }
        /* Then try STUN transaction manager */
        xStunTxnMgrOnResponse(&a->txn_mgr, &msg, buf, len, from);
      }
      break;
    }

    case XICE_DEMUX_DTLS:
      /* Feed into upper layer (PeerConnection) if DTLS hook is set */
      XDEBUG("[ice] DTLS packet %zu bytes, dtls_input_fn=%p", len,
             (void *)a->dtls_input_fn);
      if (a->dtls_input_fn) {
        a->dtls_input_fn(buf, len, from, a->dtls_input_arg);
      }
      /* else: no DTLS consumer attached, silently discard */
      break;

    case XICE_DEMUX_TURN_CHANNEL:
      /* TURN ChannelData — only if we have a TURN client */
      if (a->turn_client) {
        xTurnClientOnChannelData(a->turn_client, buf, len);
      }
      /* No TURN client — discard (per RFC 7983, this range is TURN only) */
      break;

    case XICE_DEMUX_RTP:
      /* RTP/RTCP — reserved for future use, silently discard */
      break;

    default:
      /* Unknown range — silently discard */
      break;
    }
  }
}

/* ───────────────────── Public API ───────────────────── */

xIceAgent xIceAgentCreate(xEventLoop loop, const xIceConf *conf) {
  if (!loop || !conf) return NULL;

  xIceAgent_ *a = (xIceAgent_ *)calloc(1, sizeof(xIceAgent_));
  if (!a) return NULL;

  a->conf  = *conf;
  a->loop  = loop;
  a->state = xIceAgentState_New;
  a->role  = (conf->role == xIceRole_Controlling) ? xIceAgentRole_Controlling
                                                  : xIceAgentRole_Controlled;

  /* Generate random ufrag (4+ chars) and pwd (22+ chars) */
  generate_random_string(a->ice_ufrag, XICE_UFRAG_LEN);
  generate_random_string(a->ice_pwd, XICE_PWD_LEN);

  /* Resolve birthday attack parameters:
   *   0 → use default
   *  <0 → disabled (set to 0)
   *  >0 → use specified value (clamped to hard max) */
  if (conf->birthday_k < 0) {
    a->birthday_k = 0;
  } else if (conf->birthday_k == 0) {
    a->birthday_k = XICE_BIRTHDAY_DEFAULT_K;
  } else {
    a->birthday_k =
      conf->birthday_k > XICE_BIRTHDAY_MAX_K ? XICE_BIRTHDAY_MAX_K
                                              : conf->birthday_k;
  }

  if (conf->birthday_n < 0) {
    a->birthday_n = 0;
  } else if (conf->birthday_n == 0) {
    a->birthday_n = XICE_BIRTHDAY_DEFAULT_N;
  } else {
    a->birthday_n =
      conf->birthday_n > XICE_BIRTHDAY_MAX_N ? XICE_BIRTHDAY_MAX_N
                                              : conf->birthday_n;
  }

  a->birthday_timeout_ms = conf->birthday_timeout_ms > 0
                             ? conf->birthday_timeout_ms
                             : XICE_BIRTHDAY_TIMEOUT_MS;

  xStunTxnMgrInit(&a->txn_mgr, loop);

  return (xIceAgent)a;
}

void xIceAgentDestroy(xIceAgent agent) {
  if (!agent) return;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  /* Cancel all timers */
  if (a->check_timer) {
    xEventLoopTimerCancel(a->loop, a->check_timer);
  }
  if (a->gather_timer) {
    xEventLoopTimerCancel(a->loop, a->gather_timer);
  }
  if (a->check_timeout) {
    xEventLoopTimerCancel(a->loop, a->check_timeout);
  }
  if (a->consent_timer) {
    xEventLoopTimerCancel(a->loop, a->consent_timer);
  }

  /* Clean up birthday attack state */
  cleanup_birthday(a);

  /* Cancel pending DNS queries */
  if (a->stun_dns_query) {
    xDnsCancel(a->loop, a->stun_dns_query);
    a->stun_dns_query = NULL;
  }
  if (a->turn_dns_query) {
    xDnsCancel(a->loop, a->turn_dns_query);
    a->turn_dns_query = NULL;
  }

  /* Destroy TURN client */
  if (a->turn_client) {
    xTurnClientDestroy(a->turn_client);
    free(a->turn_client);
    a->turn_client = NULL;
  }

  /* Destroy transaction manager */
  xStunTxnMgrDestroy(&a->txn_mgr);

  /* Notify Closed state before tearing down sockets, so the callback
   * still sees a consistent agent. */
  set_state(a, xIceAgentState_Closed);

  /* Close sockets — only host candidates own their sockets.
   * srflx / relay candidates share the host socket, so we must not
   * destroy the same socket twice (double-free → SIGTRAP). */
  for (int i = 0; i < a->host_count; i++) {
    if (a->local_candidates[i].sock) {
      xSocketDestroy(a->loop, a->local_candidates[i].sock);
      a->local_candidates[i].sock = NULL;
    }
  }

  /* Clear dangling sock pointers on non-host candidates */
  for (int i = a->host_count; i < a->local_count; i++) {
    a->local_candidates[i].sock = NULL;
  }

  free(a);
}

xErrno xIceAgentGather(xIceAgent agent) {
  if (!agent) return xErrno_InvalidArg;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  if (a->state != xIceAgentState_New) return xErrno_InvalidArg;

  set_state(a, xIceAgentState_Gathering);

  /*
   * Enumerate network interfaces and create a host candidate for each
   * UP, non-loopback interface (RFC 8445 §5.1.1).
   */
  struct ifaddrs *ifaddr = NULL;
  if (getifaddrs(&ifaddr) < 0) {
    set_state(a, xIceAgentState_Failed);
    return xErrno_SysError;
  }

  int iface_index = 0;
  for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    if (!(ifa->ifa_flags & IFF_UP)) continue;
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;

    sa_family_t family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;

    /* Skip IPv6 entirely when not enabled in config */
    if (family == AF_INET6 && !a->conf.enable_ipv6) continue;

    /* Skip link-local IPv6 (fe80::) — not useful for ICE */
    if (family == AF_INET6) {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)ifa->ifa_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&a6->sin6_addr)) continue;
    }

    if (a->local_count >= XICE_MAX_CANDIDATES) break;

    xSocket sock = xSocketCreate(a->loop, family, SOCK_DGRAM, 0, xEvent_Read,
                                 on_udp_recv, a);
    if (!sock) continue;

    /* Bind to this interface address with an OS-assigned port */
    struct sockaddr_storage bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    socklen_t sa_len;
    if (family == AF_INET) {
      struct sockaddr_in *dst = (struct sockaddr_in *)&bind_addr;
      struct sockaddr_in *src = (struct sockaddr_in *)ifa->ifa_addr;
      dst->sin_family         = AF_INET;
      dst->sin_addr           = src->sin_addr;
      dst->sin_port           = 0;
      sa_len                  = sizeof(struct sockaddr_in);
    } else {
      struct sockaddr_in6 *dst = (struct sockaddr_in6 *)&bind_addr;
      struct sockaddr_in6 *src = (struct sockaddr_in6 *)ifa->ifa_addr;
      dst->sin6_family         = AF_INET6;
      dst->sin6_addr           = src->sin6_addr;
      dst->sin6_scope_id       = src->sin6_scope_id;
      dst->sin6_port           = 0;
      sa_len                   = sizeof(struct sockaddr_in6);
    }

    int fd = xSocketFd(sock);
    if (bind(fd, (struct sockaddr *)&bind_addr, sa_len) < 0) {
      xSocketDestroy(a->loop, sock);
      continue;
    }

    /* Get the actual bound address (with OS-assigned port) */
    struct sockaddr_storage local_addr;
    socklen_t               addr_len = sizeof(local_addr);
    if (getsockname(fd, (struct sockaddr *)&local_addr, &addr_len) < 0) {
      xSocketDestroy(a->loop, sock);
      continue;
    }

    /* Create host candidate */
    xIceCandidate *cand = &a->local_candidates[a->local_count];
    memset(cand, 0, sizeof(*cand));
    cand->type         = xIceCandidateType_Host;
    cand->component_id = 1;
    cand->transport    = 0; /* UDP */
    cand->priority     = xIceCandidatePriority(xIceCandidateType_Host,
                                               (uint16_t)(65535 - iface_index), 1);
    memcpy(&cand->addr, &local_addr, addr_len);
    memcpy(&cand->base_addr, &local_addr, addr_len);
    cand->sock = sock;
    xIceCandidateFoundation(cand, NULL);
    a->local_count++;
    iface_index++;

    /* Notify candidate */
    if (a->conf.on_candidate) {
      char cand_line[256];
      if (xIceSdpEncodeCandidate(cand, cand_line, sizeof(cand_line)) > 0) {
        a->conf.on_candidate((xIceAgent)a, cand_line, a->conf.ctx);
      }
    }
  }

  freeifaddrs(ifaddr);

  a->host_count = a->local_count;

  if (a->host_count == 0) {
    set_state(a, xIceAgentState_Failed);
    return xErrno_SysError;
  }

  /* ── Gather srflx candidate via STUN server ── */
  if (a->conf.stun_server) {
    char     host[256];
    uint16_t port;
    if (parse_host_port(a->conf.stun_server, host, sizeof(host), &port)) {
      struct addrinfo hints;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family   = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;

      a->stun_port = port;
      a->pending_gather++;
      a->stun_dns_query =
        xDnsResolve(a->loop, host, NULL, &hints, on_stun_dns_done, a);
      if (!a->stun_dns_query) {
        /* DNS submit failed (e.g. invalid args) */
        a->pending_gather--;
      }
    }
  }

  /* ── Gather relay candidate via TURN server ── */
  if (a->conf.turn_server && a->conf.turn_username && a->conf.turn_password) {
    char     host[256];
    uint16_t port;
    if (parse_host_port(a->conf.turn_server, host, sizeof(host), &port)) {
      struct addrinfo hints;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family   = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;

      a->turn_port = port;
      a->pending_gather++;
      a->turn_dns_query =
        xDnsResolve(a->loop, host, NULL, &hints, on_turn_dns_done, a);
      if (!a->turn_dns_query) {
        a->pending_gather--;
      }
    }
  }

  /* If no pending server requests, complete gathering immediately */
  if (a->pending_gather == 0) {
    gather_check_done(a);
    return xErrno_Ok;
  }

  /* Start gathering timeout (fallback for slow servers) */
  a->gather_timer =
    xEventLoopTimerAfter(a->loop, gather_timeout_cb, a, XICE_GATHER_TIMEOUT_MS);

  return xErrno_Ok;
}

char *xIceAgentCreateOffer(xIceAgent agent) {
  if (!agent) return NULL;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  char *sdp = (char *)malloc(XSDP_MAX_SIZE);
  if (!sdp) return NULL;

  int len = xIceSdpEncode(a->ice_ufrag, a->ice_pwd, a->local_candidates,
                          a->local_count, true, sdp, XSDP_MAX_SIZE);
  if (len < 0) {
    free(sdp);
    return NULL;
  }
  sdp[len] = '\0';
  return sdp;
}

char *xIceAgentCreateAnswer(xIceAgent agent) {
  /* Same as offer for ICE purposes */
  return xIceAgentCreateOffer(agent);
}

xErrno xIceAgentSetRemoteDescription(xIceAgent agent, const char *sdp) {
  if (!agent || !sdp) return xErrno_InvalidArg;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(sdp, strlen(sdp), &parsed);
  if (err != xErrno_Ok) return err;

  strncpy(a->remote_ufrag, parsed.ice_ufrag, XICE_UFRAG_MAX_LEN - 1);
  strncpy(a->remote_pwd, parsed.ice_pwd, XICE_PWD_MAX_LEN - 1);
  a->trickle               = parsed.trickle;
  a->remote_gathering_done = parsed.end_of_candidates;
  a->remote_set            = true;

  /* Add remote candidates */
  for (int i = 0;
       i < parsed.candidate_count && a->remote_count < XICE_MAX_CANDIDATES;
       i++) {
    a->remote_candidates[a->remote_count++] = parsed.candidates[i];
  }

  /* Create TURN permissions for new remote candidates */
  turn_create_permissions_for_remotes(a);

  /* If gathering is done, start checks */
  if (a->gathering_done && a->state != xIceAgentState_Checking) {
    generate_pairs(a);
    start_checks(a);
  }

  return xErrno_Ok;
}

xErrno xIceAgentAddRemoteCandidate(xIceAgent agent, const char *candidate_sdp) {
  if (!agent || !candidate_sdp) return xErrno_InvalidArg;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  if (a->remote_count >= XICE_MAX_CANDIDATES) return xErrno_NoMemory;

  xIceCandidate cand;
  xErrno        err = xIceSdpDecodeCandidate(candidate_sdp, &cand);
  if (err != xErrno_Ok) return err;

  a->remote_candidates[a->remote_count++] = cand;

  /* Create TURN permission for the new remote candidate */
  if (a->turn_client && a->turn_client->state == xTurnState_Allocated) {
    xErrno perm_err = xTurnClientCreatePermission(
      a->turn_client, (const struct sockaddr *)&cand.addr);
    if (perm_err != xErrno_Ok) {
      XDEBUG("[ice] failed to create TURN permission for new remote candidate");
    }
  }

  /* If already checking, re-generate pairs and continue */
  if (a->state == xIceAgentState_Checking) {
    int old_count = a->pair_count;
    generate_pairs(a);

    /* Check new pairs immediately */
    for (int i = old_count; i < a->pair_count; i++) {
      if (a->pairs[i].state == xIcePairState_Frozen) {
        send_check(a, &a->pairs[i]);
      }
    }
  }

  /* If previously failed but gathering is done, restart checks with the new
   * candidate — this handles late-arriving trickle ICE candidates. */
  if (a->state == xIceAgentState_Failed && a->gathering_done) {
    a->nominated = NULL;
    generate_pairs(a);
    start_checks(a);
  }

  return xErrno_Ok;
}

xErrno xIceAgentSend(xIceAgent agent, const uint8_t *data, size_t len) {
  if (!agent || !data) return xErrno_InvalidArg;
  xIceAgent_ *a = (xIceAgent_ *)agent;

  if (a->state != xIceAgentState_Connected &&
      a->state != xIceAgentState_Completed) {
    return xErrno_InvalidArg;
  }

  if (!a->nominated || !a->nominated->local->sock) {
    return xErrno_InvalidArg;
  }

  /* Send via TURN relay if nominated pair's local candidate is relay type */
  if (a->nominated->local->type == xIceCandidateType_Relay) {
    if (!a->turn_client) return xErrno_SysError;
    return xTurnClientSendData(a->turn_client,
                               (const struct sockaddr *)&a->nominated->remote->addr,
                               data, len);
  }

  return udp_sendto(a->nominated->local->sock, data, len,
                    (const struct sockaddr *)&a->nominated->remote->addr);
}

/* ───────────────────── Accessors ───────────────────── */

const char *xIceAgentGetUfrag(xIceAgent agent) {
  if (!agent) return NULL;
  xIceAgent_ *a = (xIceAgent_ *)agent;
  return a->ice_ufrag;
}

const char *xIceAgentGetPwd(xIceAgent agent) {
  if (!agent) return NULL;
  xIceAgent_ *a = (xIceAgent_ *)agent;
  return a->ice_pwd;
}

xEventLoop xIceAgentGetLoop(xIceAgent agent) {
  if (!agent) return NULL;
  xIceAgent_ *a = (xIceAgent_ *)agent;
  return a->loop;
}

const xIceCandidate *xIceAgentGetLocalCandidates(xIceAgent agent,
                                                 int      *out_count) {
  if (!agent) {
    if (out_count) *out_count = 0;
    return NULL;
  }
  xIceAgent_ *a = (xIceAgent_ *)agent;
  if (out_count) *out_count = a->local_count;
  return a->local_candidates;
}

void xIceAgentSetDtlsInputCallback(xIceAgent agent, xIceDtlsInputFn fn,
                                   void *arg) {
  if (!agent) return;
  xIceAgent_ *a     = (xIceAgent_ *)agent;
  a->dtls_input_fn  = fn;
  a->dtls_input_arg = arg;
}

void xIceAgentSetRole(xIceAgent agent, xIceRole role) {
  if (!agent) return;
  xIceAgent_ *a = (xIceAgent_ *)agent;
  a->role       = (role == xIceRole_Controlling) ? xIceAgentRole_Controlling
                                                 : xIceAgentRole_Controlled;
}
