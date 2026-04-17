/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_birthday.c - NAT detection and birthday attack implementation
 */

#include "ice_birthday.h"
#include "ice_agent.h"
#include "ice_candidate.h"
#include "ice_private.h"
#include "stun_attr.h"
#include "stun_msg.h"
#include "stun_txn.h"

#include <xbase/log.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ───────────────────── Helpers ───────────────────── */

static socklen_t sockaddr_len(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) return sizeof(struct sockaddr_in);
  if (addr->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
  return 0;
}

static void sockaddr_set_port(struct sockaddr_storage *addr, uint16_t port) {
  if (addr->ss_family == AF_INET) {
    ((struct sockaddr_in *)addr)->sin_port = htons(port);
  } else if (addr->ss_family == AF_INET6) {
    ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
  }
}

static xErrno udp_sendto(xSocket sock, const uint8_t *data, size_t len,
                         const struct sockaddr *addr) {
  int fd = xSocketFd(sock);
  if (fd < 0) return xErrno_SysError;
  ssize_t n = sendto(fd, data, len, 0, addr, sockaddr_len(addr));
  return (n >= 0) ? xErrno_Ok : xErrno_SysError;
}

/* ───────────────────── NAT Detection ───────────────────── */

/**
 * @brief Context for a single NAT detection STUN request.
 */
typedef struct {
  xIceNatDetectCtx *detect; /**< Parent detection context.  */
  xSocket           sock;   /**< Socket used for this probe. */
  int               index;  /**< Index in mapped_ports array. */
} NatProbeCtx;

/**
 * @brief Send function for NAT detection probes.
 */
static xErrno nat_probe_send(const uint8_t *data, size_t len,
                             const struct sockaddr *addr, void *arg) {
  xSocket sock = (xSocket)arg;
  return udp_sendto(sock, data, len, addr);
}

static const char *nat_type_str[] = {
  [xNatType_Unknown]             = "unknown",
  [xNatType_Cone]                = "cone",
  [xNatType_SymmetricSequential] = "symmetric-sequential",
  [xNatType_SymmetricRandom]     = "symmetric-random",
};

/**
 * @brief Timeout callback for NAT detection.
 *
 * Fires when the detection timeout expires before all probes complete.
 * Classifies NAT based on whatever responses have been collected so far.
 */
static void nat_detect_timeout_cb(void *arg) {
  xIceNatDetectCtx *detect = (xIceNatDetectCtx *)arg;
  xIceAgent_       *agent  = detect->agent;
  detect->timer             = NULL;

  if (detect->done) return;
  detect->done = true;

  XDEBUG("[ice] NAT detect: timeout (%d/%d responses)",
         detect->responses, XICE_NAT_DETECT_SOCKETS);

  xNatType nat_type;
  if (detect->responses >= 2) {
    nat_type =
      xIceBirthdayClassifyNat(detect->mapped_ports, detect->responses);
  } else {
    nat_type = xNatType_Unknown;
  }

  agent->local_nat_type = nat_type;
  XDEBUG("[ice] NAT type detected: %s (timeout)", nat_type_str[nat_type]);

  /* Note: probe sockets and STUN transactions will be cleaned up
   * naturally when their individual timeouts fire. The detect context
   * is kept alive until all pending probes complete. */
}

/**
 * @brief Callback for NAT detection STUN responses. */
static void on_nat_probe_response(const xStunMsg        *msg,
                                  const struct sockaddr *from
                                  __attribute__((unused)),
                                  void *arg) {
  NatProbeCtx      *probe  = (NatProbeCtx *)arg;
  xIceNatDetectCtx *detect = probe->detect;
  xIceAgent_       *agent  = detect->agent;

  if (detect->done && msg && xStunMsgIsSuccessResponse(msg->type)) {
    /* Detection already completed (by timeout), just clean up */
    xSocketDestroy(agent->loop, probe->sock);
    probe->sock = NULL;
    detect->pending--;
    if (detect->pending == 0) free(detect);
    free(probe);
    return;
  }

  if (msg && xStunMsgIsSuccessResponse(msg->type)) {
    /* Extract XOR-MAPPED-ADDRESS to get mapped port */
    xStunAttrIter iter;
    xStunAttrIterInit(&iter, msg);
    xStunAttr attr;

    while (xStunAttrIterNext(&iter, &attr)) {
      if (attr.type == xStunAttrType_XorMappedAddress) {
        struct sockaddr_storage mapped;
        if (xStunAttrDecodeXorMappedAddress(&attr, msg->txn_id, &mapped) ==
            xErrno_Ok) {
          uint16_t port = xSockaddrPort((const struct sockaddr *)&mapped);
          detect->mapped_ports[probe->index] = port;
          detect->responses++;
          XDEBUG("[ice] NAT detect: probe %d mapped port = %u", probe->index,
                 port);
        }
        break;
      }
    }
  }

  /* Destroy the probe socket (it was only for detection) */
  xSocketDestroy(agent->loop, probe->sock);
  probe->sock = NULL;

  detect->pending--;

  /* All probes done — classify NAT type */
  if (detect->pending == 0) {
    if (!detect->done) {
      detect->done = true;

      /* Cancel the detection timeout timer */
      if (detect->timer) {
        xEventLoopTimerCancel(agent->loop, detect->timer);
        detect->timer = NULL;
      }

      xNatType nat_type;
      if (detect->responses >= 2) {
        nat_type =
          xIceBirthdayClassifyNat(detect->mapped_ports, detect->responses);
      } else {
        nat_type = xNatType_Unknown;
      }

      agent->local_nat_type = nat_type;
      xLog(false, "[ice] NAT type detected: %s", nat_type_str[nat_type]);
    }

    free(detect);
  }

  free(probe);
}

/**
 * @brief Callback for NAT detection socket I/O events.
 *
 * Routes incoming STUN responses to the transaction manager.
 */
static void on_nat_detect_recv(xSocket sock, xEventMask mask, void *arg) {
  xIceAgent_ *a = (xIceAgent_ *)arg;

  if (!(mask & xEvent_Read)) return;

  int fd = xSocketFd(sock);

  for (;;) {
    uint8_t                 buf[256];
    struct sockaddr_storage from_addr;
    socklen_t               from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr,
                         &from_len);
    if (n <= 0) break;

    size_t len = (size_t)n;
    if (!xStunMsgIsStun(buf, len)) continue;

    xStunMsg msg;
    if (xStunMsgDecode(&msg, buf, len) != xErrno_Ok) continue;

    if (xStunMsgIsSuccessResponse(msg.type) ||
        xStunMsgIsErrorResponse(msg.type)) {
      xStunTxnMgrOnResponse(&a->txn_mgr, &msg, buf, len,
                            (const struct sockaddr *)&from_addr);
    }
  }
}

xNatType xIceBirthdayClassifyNat(const uint16_t *ports, int count) {
  if (count < 2) return xNatType_Unknown;

  /* Check if all ports are the same → Cone NAT */
  bool all_same = true;
  for (int i = 1; i < count; i++) {
    if (ports[i] != ports[0]) {
      all_same = false;
      break;
    }
  }
  if (all_same) return xNatType_Cone;

  /* Check for sequential pattern: differences are small and consistent */
  bool sequential = true;
  int  max_diff   = 0;
  for (int i = 1; i < count; i++) {
    int diff = abs((int)ports[i] - (int)ports[i - 1]);
    if (diff > max_diff) max_diff = diff;
    /* If the difference between consecutive ports is > 20,
     * it's unlikely to be a sequential allocator */
    if (diff > 20) {
      sequential = false;
      break;
    }
  }

  if (sequential && max_diff > 0) return xNatType_SymmetricSequential;

  return xNatType_SymmetricRandom;
}

xErrno xIceBirthdayDetectNat(xIceAgent_                    *agent,
                             const struct sockaddr_storage *stun_addr) {
  if (!agent || !stun_addr) return xErrno_InvalidArg;

  /* Find a host candidate to determine the local bind address */
  const struct sockaddr *bind_addr = NULL;
  for (int i = 0; i < agent->host_count; i++) {
    if (agent->local_candidates[i].addr.ss_family == stun_addr->ss_family) {
      bind_addr = (const struct sockaddr *)&agent->local_candidates[i].addr;
      break;
    }
  }
  if (!bind_addr) return xErrno_InvalidArg;

  /* Allocate detection context */
  xIceNatDetectCtx *detect =
    (xIceNatDetectCtx *)calloc(1, sizeof(xIceNatDetectCtx));
  if (!detect) return xErrno_NoMemory;
  detect->agent     = agent;
  detect->responses = 0;
  detect->pending   = 0;
  detect->timer     = NULL;
  detect->done      = false;

  XDEBUG("[ice] NAT detect: starting with %d probes", XICE_NAT_DETECT_SOCKETS);

  /* Create probe sockets and send STUN Binding Requests */
  for (int i = 0; i < XICE_NAT_DETECT_SOCKETS; i++) {
    xSocket sock =
      xSocketCreate(agent->loop, (int)stun_addr->ss_family, SOCK_DGRAM, 0,
                    xEvent_Read, on_nat_detect_recv, agent);
    if (!sock) continue;

    /* Bind to the same local interface with OS-assigned port */
    int                     fd = xSocketFd(sock);
    struct sockaddr_storage local_bind;
    memcpy(&local_bind, bind_addr, sizeof(local_bind));
    sockaddr_set_port(&local_bind, 0);
    if (bind(fd, (const struct sockaddr *)&local_bind,
             sockaddr_len((const struct sockaddr *)&local_bind)) < 0) {
      xSocketDestroy(agent->loop, sock);
      continue;
    }

    /* Build STUN Binding Request */
    uint8_t msg_buf[256];
    uint8_t txn_id[XSTUN_TXN_ID_SIZE];
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
    for (int j = 0; j < XSTUN_TXN_ID_SIZE; j++)
      txn_id[j] = (uint8_t)(rand() & 0xFF);
#endif
    xStunMsg stun_msg;
    xStunMsgInit(&stun_msg, xStunMsgType_BindingRequest, txn_id);
    xStunMsgEncode(&stun_msg, msg_buf, sizeof(msg_buf));
    xWriteU16BE(msg_buf + 2, 0); /* No attributes */

    /* Allocate probe context */
    NatProbeCtx *probe = (NatProbeCtx *)malloc(sizeof(NatProbeCtx));
    if (!probe) {
      xSocketDestroy(agent->loop, sock);
      continue;
    }
    probe->detect = detect;
    probe->sock   = sock;
    probe->index  = detect->pending;

    xErrno err = xStunTxnMgrSendRaw(
      &agent->txn_mgr, msg_buf, XSTUN_HEADER_SIZE, (struct sockaddr *)stun_addr,
      nat_probe_send, sock, on_nat_probe_response, probe);

    if (err != xErrno_Ok) {
      xSocketDestroy(agent->loop, sock);
      free(probe);
      continue;
    }

    detect->pending++;
  }

  if (detect->pending == 0) {
    /* Failed to send any probes */
    free(detect);
    agent->local_nat_type = xNatType_Unknown;
    return xErrno_SysError;
  }

  /* Start detection timeout timer */
  detect->timer = xEventLoopTimerAfter(agent->loop, nat_detect_timeout_cb,
                                       detect, XICE_NAT_DETECT_TIMEOUT_MS);

  XDEBUG("[ice] NAT detect: %d probes sent, timeout=%dms",
         detect->pending, XICE_NAT_DETECT_TIMEOUT_MS);

  return xErrno_Ok;
}

/* ───────────────────── Birthday Probe Builder ───────────────────── */

size_t xIceBirthdayBuildProbe(uint8_t *buf) {
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

/* ───────────────────── Birthday Attack Lifecycle ───────────────────── */

void xIceBirthdayInit(xIceBirthdayCtx *ctx, xIceAgent_ *agent, int k, int n,
                      int timeout_ms) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->agent      = agent;
  ctx->state      = xIceBirthdayState_Idle;
  ctx->k          = k;
  ctx->n          = n;
  ctx->timeout_ms = timeout_ms;
}

void xIceBirthdayCleanup(xIceBirthdayCtx *ctx) {
  if (!ctx) return;

  xIceAgent_ *a = ctx->agent;
  if (!a) return;

  if (ctx->pacing_timer) {
    xEventLoopTimerCancel(a->loop, ctx->pacing_timer);
    ctx->pacing_timer = NULL;
  }
  if (ctx->timeout_timer) {
    xEventLoopTimerCancel(a->loop, ctx->timeout_timer);
    ctx->timeout_timer = NULL;
  }

  for (int i = 0; i < ctx->sock_count; i++) {
    if (ctx->socks[i]) {
      xSocketDestroy(a->loop, ctx->socks[i]);
      ctx->socks[i] = NULL;
    }
  }

  if (ctx->socks) {
    free(ctx->socks);
    ctx->socks = NULL;
  }

  ctx->sock_count  = 0;
  ctx->burst_index = 0;
  ctx->state       = xIceBirthdayState_Done;
}

/* ───────────────────── Birthday Attack State Machine ───────────────────── */

/* Forward declarations */
static void     birthday_pacing_cb(void *arg);
static void     birthday_timeout_cb(void *arg);
static void     on_birthday_recv(xSocket sock, xEventMask mask, void *arg);
static xErrno   create_birthday_sockets(xIceBirthdayCtx *ctx);
static void     start_probing(xIceBirthdayCtx *ctx);
static uint16_t pick_target_port(xIceBirthdayCtx *ctx);

/**
 * @brief Fire the birthday signal callback to send a message via signaling.
 */
static void fire_signal(xIceBirthdayCtx *ctx, xIceBirthdaySignalType type) {
  xIceAgent_ *a = ctx->agent;
  if (!a->conf.on_birthday_signal) return;

  xIceBirthdaySignal sig;
  memset(&sig, 0, sizeof(sig));
  sig.type       = type;
  sig.k          = ctx->k;
  sig.n          = ctx->n;
  sig.timeout_ms = ctx->timeout_ms;

  a->conf.on_birthday_signal((xIceAgent)a, &sig, a->conf.ctx);
}

/**
 * @brief Transition the agent to Connected state after a birthday hit.
 */
static void birthday_hit(xIceBirthdayCtx *ctx, xSocket hit_sock,
                         const struct sockaddr_storage *from) {
  xIceAgent_ *a = ctx->agent;

  XDEBUG("[ice] birthday attack: HIT! from port %u",
         xSockaddrPort((const struct sockaddr *)from));
  (void)from;

  /* Stop timers and close non-hit sockets */
  if (ctx->pacing_timer) {
    xEventLoopTimerCancel(a->loop, ctx->pacing_timer);
    ctx->pacing_timer = NULL;
  }
  if (ctx->timeout_timer) {
    xEventLoopTimerCancel(a->loop, ctx->timeout_timer);
    ctx->timeout_timer = NULL;
  }

  for (int i = 0; i < ctx->sock_count; i++) {
    if (ctx->socks[i] && ctx->socks[i] != hit_sock) {
      xSocketDestroy(a->loop, ctx->socks[i]);
      ctx->socks[i] = NULL;
    }
  }

  ctx->state = xIceBirthdayState_Done;

  /* Create a new candidate pair from the hit socket and promote it.
   * For simplicity, we add the hit socket as a new host candidate
   * and create a pair with the remote srflx candidate. */
  if (a->local_count < XICE_MAX_CANDIDATES) {
    xIceCandidate *cand = &a->local_candidates[a->local_count];
    memset(cand, 0, sizeof(*cand));
    cand->type         = xIceCandidateType_Host;
    cand->component_id = 1;
    cand->sock         = hit_sock;
    snprintf(cand->foundation, XICE_FOUNDATION_MAX_LEN, "bday%d",
             a->local_count);

    /* Get local address from socket */
    struct sockaddr_storage local_addr;
    socklen_t               addr_len = sizeof(local_addr);
    int                     fd       = xSocketFd(hit_sock);
    if (getsockname(fd, (struct sockaddr *)&local_addr, &addr_len) == 0) {
      memcpy(&cand->addr, &local_addr, sizeof(local_addr));
    }

    cand->priority = xIceCandidatePriority(cand->type, 1, cand->component_id);
    a->local_count++;

    /* Find the remote srflx candidate to pair with */
    xIceCandidate *remote_cand = NULL;
    for (int i = 0; i < a->remote_count; i++) {
      if (a->remote_candidates[i].type == xIceCandidateType_Srflx) {
        remote_cand = &a->remote_candidates[i];
        break;
      }
    }
    if (!remote_cand && a->remote_count > 0) {
      remote_cand = &a->remote_candidates[0];
    }

    if (remote_cand && a->pair_count < XICE_MAX_PAIRS) {
      xIcePair *pair = &a->pairs[a->pair_count];
      memset(pair, 0, sizeof(*pair));
      pair->local     = cand;
      pair->remote    = remote_cand;
      pair->priority  = xIcePairPriority(cand->priority, remote_cand->priority);
      pair->state     = xIcePairState_Succeeded;
      pair->nominated = true;
      a->pair_count++;

      a->nominated = pair;
    }
  }

  /* Remove the hit socket from the birthday context so cleanup
   * doesn't destroy it */
  for (int i = 0; i < ctx->sock_count; i++) {
    if (ctx->socks[i] == hit_sock) {
      ctx->socks[i] = NULL;
      break;
    }
  }

  xIceAgentSetStateInternal(a, xIceAgentState_Connected);
}

/**
 * @brief Receive callback for birthday attack sockets.
 *
 * Checks if we received a STUN Binding Request (from the peer's
 * birthday probes) and responds with a Binding Response.
 */
static void on_birthday_recv(xSocket sock, xEventMask mask, void *arg) {
  xIceBirthdayCtx *ctx = (xIceBirthdayCtx *)arg;

  if (!(mask & xEvent_Read)) return;
  if (ctx->state != xIceBirthdayState_Probing) return;

  int fd = xSocketFd(sock);

  for (;;) {
    uint8_t                 buf[256];
    struct sockaddr_storage from_addr;
    socklen_t               from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr,
                         &from_len);
    if (n <= 0) break;

    size_t len = (size_t)n;
    if (!xStunMsgIsStun(buf, len)) continue;

    xStunMsg msg;
    if (xStunMsgDecode(&msg, buf, len) != xErrno_Ok) continue;

    if (xStunMsgIsRequest(msg.type)) {
      /* Received a STUN Binding Request from the peer — this is a HIT! */
      XDEBUG("[ice] birthday: received STUN request from peer");

      /* Send a simple Binding Response (header only, no attributes) */
      uint8_t  resp_buf[XSTUN_HEADER_SIZE];
      xStunMsg resp;
      xStunMsgInit(&resp, xStunMsgType_BindingResponse, msg.txn_id);
      int resp_len = xStunMsgEncode(&resp, resp_buf, sizeof(resp_buf));
      if (resp_len > 0) {
        sendto(fd, resp_buf, resp_len, 0, (const struct sockaddr *)&from_addr,
               from_len);
      }

      birthday_hit(ctx, sock, &from_addr);
      return;
    }

    if (xStunMsgIsSuccessResponse(msg.type)) {
      /* Received a Binding Response — our probe hit the peer's socket! */
      XDEBUG("[ice] birthday: received STUN response (our probe hit)");
      birthday_hit(ctx, sock, &from_addr);
      return;
    }
  }
}

/**
 * @brief Create k birthday attack sockets.
 */
static xErrno create_birthday_sockets(xIceBirthdayCtx *ctx) {
  xIceAgent_ *a = ctx->agent;

  ctx->socks = (xSocket *)calloc(ctx->k, sizeof(xSocket));
  if (!ctx->socks) return xErrno_NoMemory;

  /* Determine address family from first host candidate */
  int family = AF_INET;
  if (a->host_count > 0) {
    family = (int)a->local_candidates[0].addr.ss_family;
  }

  /* Get local bind address from first host candidate */
  struct sockaddr_storage local_bind;
  memset(&local_bind, 0, sizeof(local_bind));
  if (a->host_count > 0) {
    memcpy(&local_bind, &a->local_candidates[0].addr, sizeof(local_bind));
  } else {
    local_bind.ss_family = family;
  }
  sockaddr_set_port(&local_bind, 0); /* OS-assigned port */

  int created = 0;
  for (int i = 0; i < ctx->k; i++) {
    xSocket sock = xSocketCreate(a->loop, family, SOCK_DGRAM, 0, xEvent_Read,
                                 on_birthday_recv, ctx);
    if (!sock) continue;

    int fd = xSocketFd(sock);
    if (bind(fd, (const struct sockaddr *)&local_bind,
             sockaddr_len((const struct sockaddr *)&local_bind)) < 0) {
      xSocketDestroy(a->loop, sock);
      continue;
    }

    ctx->socks[created++] = sock;
  }

  ctx->sock_count = created;
  XDEBUG("[ice] birthday attack: created %d/%d sockets", created, ctx->k);

  return (created > 0) ? xErrno_Ok : xErrno_SysError;
}

/**
 * @brief Pick a target port based on NAT type and probing strategy.
 */
static uint16_t pick_target_port(xIceBirthdayCtx *ctx) {
  xIceAgent_ *a = ctx->agent;

  if (a->remote_nat_type == xNatType_SymmetricSequential &&
      ctx->target_port > 0) {
    /* Sequential mode: scan ±200 around the srflx port */
    int base  = (int)ctx->target_port;
    int range = 200;
    int low   = base - range;
    int high  = base + range;
    if (low < 1024) low = 1024;
    if (high > 65535) high = 65535;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return (uint16_t)(low + (int)arc4random_uniform(high - low + 1));
#else
    return (uint16_t)(low + rand() % (high - low + 1));
#endif
  }

  /* Random mode: pick from ephemeral range 32768-65535 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  return (uint16_t)(32768 + (int)arc4random_uniform(32768));
#else
  return (uint16_t)(32768 + rand() % 32768);
#endif
}

/**
 * @brief Pacing timer callback — sends one burst of probes.
 *
 * Each burst sends one probe from each socket to a random target port.
 */
static void birthday_pacing_cb(void *arg) {
  xIceBirthdayCtx *ctx = (xIceBirthdayCtx *)arg;
  xIceAgent_      *a   = ctx->agent;
  ctx->pacing_timer    = NULL;

  if (ctx->state != xIceBirthdayState_Probing) return;

  /* Build a STUN Binding Request probe */
  uint8_t probe[XSTUN_HEADER_SIZE];
  xIceBirthdayBuildProbe(probe);

  /* Send from each socket to a random port */
  struct sockaddr_storage target;
  memcpy(&target, &ctx->target, sizeof(target));

  for (int i = 0; i < ctx->sock_count; i++) {
    if (!ctx->socks[i]) continue;

    uint16_t port = pick_target_port(ctx);
    sockaddr_set_port(&target, port);

    udp_sendto(ctx->socks[i], probe, XSTUN_HEADER_SIZE,
               (const struct sockaddr *)&target);
  }

  ctx->burst_index++;

  /* Schedule next burst if we haven't exhausted n */
  if (ctx->burst_index < ctx->n) {
    ctx->pacing_timer = xEventLoopTimerAfter(a->loop, birthday_pacing_cb, ctx,
                                             XICE_BIRTHDAY_PACING_MS);
  }
}

/**
 * @brief Overall timeout callback — birthday attack failed.
 */
static void birthday_timeout_cb(void *arg) {
  xIceBirthdayCtx *ctx = (xIceBirthdayCtx *)arg;
  xIceAgent_      *a   = ctx->agent;
  ctx->timeout_timer   = NULL;

  XDEBUG("[ice] birthday attack timed out");

  xIceBirthdayCleanup(ctx);
  xIceAgentSetStateInternal(a, xIceAgentState_Failed);
}

/**
 * @brief Start the probing phase (both sides call this simultaneously).
 */
static void start_probing(xIceBirthdayCtx *ctx) {
  xIceAgent_ *a = ctx->agent;

  ctx->state = xIceBirthdayState_Probing;

  /* Determine target address from remote srflx candidate */
  bool found_target = false;
  for (int i = 0; i < a->remote_count; i++) {
    if (a->remote_candidates[i].type == xIceCandidateType_Srflx) {
      memcpy(&ctx->target, &a->remote_candidates[i].addr, sizeof(ctx->target));
      ctx->target_port =
        xSockaddrPort((const struct sockaddr *)&a->remote_candidates[i].addr);
      found_target = true;
      break;
    }
  }

  if (!found_target && a->remote_count > 0) {
    /* Fallback to first remote candidate */
    memcpy(&ctx->target, &a->remote_candidates[0].addr, sizeof(ctx->target));
    ctx->target_port =
      xSockaddrPort((const struct sockaddr *)&a->remote_candidates[0].addr);
    found_target = true;
  }

  if (!found_target) {
    XDEBUG("[ice] birthday: no remote candidate for target");
    xIceBirthdayCleanup(ctx);
    xIceAgentSetStateInternal(a, xIceAgentState_Failed);
    return;
  }

  XDEBUG("[ice] birthday attack: starting probing, target port=%u, "
         "k=%d, n=%d, timeout=%dms",
         ctx->target_port, ctx->k, ctx->n, ctx->timeout_ms);

  /* Create sockets */
  if (create_birthday_sockets(ctx) != xErrno_Ok) {
    XDEBUG("[ice] birthday: failed to create sockets");
    xIceBirthdayCleanup(ctx);
    xIceAgentSetStateInternal(a, xIceAgentState_Failed);
    return;
  }

  /* Start overall timeout */
  ctx->timeout_timer =
    xEventLoopTimerAfter(a->loop, birthday_timeout_cb, ctx, ctx->timeout_ms);

  /* Start first burst immediately */
  ctx->burst_index = 0;
  birthday_pacing_cb(ctx);
}

/* ───────────────────── State Machine Handlers ───────────────────── */

xErrno xIceBirthdayStartAsController(xIceBirthdayCtx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  XDEBUG("[ice] birthday: controller sending Init");
  ctx->state = xIceBirthdayState_WaitReady;

  /* Send birthday_init to the peer */
  fire_signal(ctx, xIceBirthdaySignal_Init);

  return xErrno_Ok;
}

xErrno xIceBirthdayHandleSignal(xIceBirthdayCtx          *ctx,
                                const xIceBirthdaySignal *signal) {
  if (!ctx || !signal) return xErrno_InvalidArg;

  xIceAgent_ *a = ctx->agent;

  switch (signal->type) {
  case xIceBirthdaySignal_Init:
    /* Controlled side received Init from controller */
    XDEBUG("[ice] birthday: received Init (k=%d n=%d t=%d)", signal->k,
           signal->n, signal->timeout_ms);

    /* Use the controller's parameters */
    ctx->k          = signal->k;
    ctx->n          = signal->n;
    ctx->timeout_ms = signal->timeout_ms;

    /* Create sockets and reply Ready */
    if (create_birthday_sockets(ctx) != xErrno_Ok) {
      XDEBUG("[ice] birthday: failed to create sockets for controlled side");
      xIceAgentSetStateInternal(a, xIceAgentState_Failed);
      return xErrno_SysError;
    }

    ctx->state = xIceBirthdayState_WaitGo;
    fire_signal(ctx, xIceBirthdaySignal_Ready);
    break;

  case xIceBirthdaySignal_Ready:
    /* Controller received Ready from controlled side */
    if (ctx->state != xIceBirthdayState_WaitReady) {
      XDEBUG("[ice] birthday: unexpected Ready in state %d", ctx->state);
      return xErrno_InvalidState;
    }

    XDEBUG("[ice] birthday: received Ready, sending Go");

    /* Create our sockets too */
    if (ctx->sock_count == 0) {
      if (create_birthday_sockets(ctx) != xErrno_Ok) {
        XDEBUG("[ice] birthday: failed to create sockets for controller");
        xIceAgentSetStateInternal(a, xIceAgentState_Failed);
        return xErrno_SysError;
      }
    }

    /* Send Go and start probing */
    fire_signal(ctx, xIceBirthdaySignal_Go);
    start_probing(ctx);
    break;

  case xIceBirthdaySignal_Go:
    /* Controlled side received Go — start probing */
    if (ctx->state != xIceBirthdayState_WaitGo) {
      XDEBUG("[ice] birthday: unexpected Go in state %d", ctx->state);
      return xErrno_InvalidState;
    }

    XDEBUG("[ice] birthday: received Go, starting probing");

    /* Determine target and start probing */
    {
      bool found_target = false;
      for (int i = 0; i < a->remote_count; i++) {
        if (a->remote_candidates[i].type == xIceCandidateType_Srflx) {
          memcpy(&ctx->target, &a->remote_candidates[i].addr,
                 sizeof(ctx->target));
          ctx->target_port = xSockaddrPort(
            (const struct sockaddr *)&a->remote_candidates[i].addr);
          found_target = true;
          break;
        }
      }
      if (!found_target && a->remote_count > 0) {
        memcpy(&ctx->target, &a->remote_candidates[0].addr,
               sizeof(ctx->target));
        ctx->target_port =
          xSockaddrPort((const struct sockaddr *)&a->remote_candidates[0].addr);
        found_target = true;
      }
      if (!found_target) {
        XDEBUG("[ice] birthday: no remote candidate for target");
        xIceBirthdayCleanup(ctx);
        xIceAgentSetStateInternal(a, xIceAgentState_Failed);
        return xErrno_InvalidArg;
      }
    }

    ctx->state = xIceBirthdayState_Probing;

    /* Start overall timeout */
    ctx->timeout_timer =
      xEventLoopTimerAfter(a->loop, birthday_timeout_cb, ctx, ctx->timeout_ms);

    /* Start first burst */
    ctx->burst_index = 0;
    birthday_pacing_cb(ctx);
    break;
  }

  return xErrno_Ok;
}
