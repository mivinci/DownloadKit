/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nat_probe.c - NAT type detection via STUN (RFC 5389)
 *
 * Two-phase detection:
 *   Phase 1: 1 socket → 2 STUN servers  → Cone vs Symmetric
 *   Phase 2: 3 sockets → STUN server A  → Sequential vs Random
 */

#include "nat_probe.h"
#include "stun_attr.h"
#include "stun_msg.h"
#include <x/base/log.h>
#include <x/net/dns.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ───────────────────── Constants ───────────────────── */

/** Number of STUN servers for Phase 1. */
#define XNAT_PHASE1_COUNT 2

/** Number of independent sockets for Phase 2. */
#define XNAT_PHASE2_COUNT 3

/** Default per-request timeout in milliseconds. */
#define XNAT_PROBE_DEFAULT_TIMEOUT_MS 3000

/* ───────────────────── Forward declarations ───────────────────── */

static void nat_probe_gen_txn_id(uint8_t txn_id[XSTUN_TXN_ID_SIZE]);

/* ───────────────────── Internal structures ───────────────────── */

/**
 * @brief State for a single STUN test within a probe.
 */
XDEF_STRUCT(xNatProbeTest) {
  int      fd;                         /**< UDP socket fd (-1 if unused).   */
  xSocket  sock;                       /**< Async socket handle.            */
  bool     done;                       /**< Response received.              */
  bool     failed;                     /**< Timed out or error.             */
  uint16_t mapped_port;                /**< Mapped port from STUN response. */
  uint8_t  txn_id[XSTUN_TXN_ID_SIZE]; /**< Transaction ID for matching.    */
  struct xNatProbe_ *probe;        /**< Back-pointer to parent probe.   */
};

/**
 * @brief Probe phase.
 */
XDEF_ENUM(xNatProbePhase) {
  xNatProbePhase_DNS1 = 0, /**< Resolving first STUN server.  */
  xNatProbePhase_DNS2,     /**< Resolving second STUN server. */
  xNatProbePhase_Phase1,   /**< Phase 1: Cone vs Symmetric.   */
  xNatProbePhase_Phase2,   /**< Phase 2: Sequential vs Random.*/
};

/**
 * @brief Internal NAT probe state.
 */
XDEF_STRUCT(xNatProbe_) {
  xEventLoop    loop;
  xNatProbeFunc cb;
  void         *arg;

  /* STUN server addresses (resolved). */
  struct sockaddr_storage stun_addr1; /**< First STUN server address.  */
  struct sockaddr_storage stun_addr2; /**< Second STUN server address. */
  uint16_t stun_port1;
  uint16_t stun_port2;
  char    *stun_host2; /**< Saved for deferred DNS resolution. */

  xNatProbePhase phase;

  /* Phase 1: 1 socket, 2 tests (one per STUN server). */
  xNatProbeTest phase1_tests[XNAT_PHASE1_COUNT];
  int           phase1_done_count;
  int           phase1_fd; /**< Shared socket fd for phase 1. */

  /* Phase 2: 3 sockets, 3 tests (all to STUN server A). */
  xNatProbeTest phase2_tests[XNAT_PHASE2_COUNT];
  int           phase2_done_count;

  xDnsQuery     dns_query; /**< Pending DNS query handle.         */
  int           timeout_ms;
  xEventTimer   timeout_timer; /**< Overall timeout timer.        */
  bool          finished;      /**< Probe already completed/cancelled. */
};

/* ───────────────────── Forward declarations ───────────────────── */

static void nat_probe_check_phase1_done(xNatProbe_ *p);
static void nat_probe_check_phase2_done(xNatProbe_ *p);
static void nat_probe_finish(xNatProbe_ *p, bool timed_out);
static void nat_probe_free(xNatProbe_ *p);
static bool nat_probe_start_phase1(xNatProbe_ *p);
static bool nat_probe_start_phase2(xNatProbe_ *p);
static bool nat_probe_send_request_to(xNatProbe_ *p, xNatProbeTest *t,
                                      int fd,
                                      struct sockaddr_storage *addr);
static void nat_probe_phase1_on_readable(xSocket sock, xEventMask mask,
                                         void *arg);

/* ───────────────────── Parse STUN response (shared) ───────────────────── */

/**
 * @brief Parse a STUN Binding Response and extract the mapped port.
 * @return true on success (mapped_port is set), false on failure.
 */
static bool nat_probe_parse_response(const uint8_t *buf, size_t len,
                                     const uint8_t txn_id[XSTUN_TXN_ID_SIZE],
                                     uint16_t *out_port) {
  if (!xStunMsgIsStun(buf, len)) return false;

  xStunMsg msg;
  if (xStunMsgDecode(&msg, buf, len) != xErrno_Ok) return false;
  if (memcmp(msg.txn_id, txn_id, XSTUN_TXN_ID_SIZE) != 0) return false;
  if (!xStunMsgIsSuccessResponse(msg.type)) return false;

  xStunAttrIter iter;
  xStunAttr     attr;
  xStunAttrIterInit(&iter, &msg);

  struct sockaddr_storage mapped;
  bool                    found = false;

  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_XorMappedAddress) {
      if (xStunAttrDecodeXorMappedAddress(&attr, msg.txn_id, &mapped) ==
          xErrno_Ok) {
        found = true;
        break;
      }
    } else if (attr.type == xStunAttrType_MappedAddress && !found) {
      if (xStunAttrDecodeMappedAddress(&attr, &mapped) == xErrno_Ok) {
        found = true;
        /* Don't break — prefer XOR-MAPPED-ADDRESS if it comes later. */
      }
    }
  }

  if (!found) return false;

  if (mapped.ss_family == AF_INET) {
    *out_port = ntohs(((struct sockaddr_in *)&mapped)->sin_port);
  } else if (mapped.ss_family == AF_INET6) {
    *out_port = ntohs(((struct sockaddr_in6 *)&mapped)->sin6_port);
  } else {
    return false;
  }
  return true;
}

/* Phase 1 read callback is defined below nat_probe_start_phase1(). */

/* ───────────────────── Phase 2 read callback ───────────────────── */

static void nat_probe_phase2_on_readable(xSocket sock, xEventMask mask,
                                         void *arg) {
  xNatProbeTest  *t = (xNatProbeTest *)arg;
  xNatProbe_ *p = t->probe;
  int             idx = (int)(t - p->phase2_tests);

  (void)sock;
  (void)idx;

  if (p->finished) return;
  if (t->done || t->failed) return;

  if (mask & xEvent_Timeout) {
    XDEBUGL1("[nat-probe] phase2 test[%d] timed out", idx);
    t->failed = true;
    p->phase2_done_count++;
    nat_probe_check_phase2_done(p);
    return;
  }

  uint8_t                 buf[XSTUN_MAX_MSG_SIZE];
  struct sockaddr_storage from;
  socklen_t               from_len = sizeof(from);

  ssize_t n =
    recvfrom(t->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
  if (n <= 0) return;

  if (!nat_probe_parse_response(buf, (size_t)n, t->txn_id, &t->mapped_port)) {
    return;
  }

  XDEBUGL1("[nat-probe] phase2 test[%d] mapped port = %u", idx,
           (unsigned)t->mapped_port);

  t->done = true;
  p->phase2_done_count++;
  nat_probe_check_phase2_done(p);
}

/* ───────────────────── Overall timeout ───────────────────── */

static void nat_probe_on_timeout(void *arg) {
  xNatProbe_ *p = (xNatProbe_ *)arg;
  if (p->finished) return;
  XDEBUGL0("[nat-probe] overall timeout reached");
  nat_probe_finish(p, true);
}

/* ───────────────────── Check Phase 1 completion ───────────────────── */

static void nat_probe_check_phase1_done(xNatProbe_ *p) {
  if (p->phase1_done_count < XNAT_PHASE1_COUNT) return;

  /* Both phase 1 tests done. Check results. */
  bool all_ok = true;
  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    if (!p->phase1_tests[i].done) {
      all_ok = false;
      break;
    }
  }

  if (!all_ok) {
    XDEBUGL0("[nat-probe] phase1 incomplete — some tests failed");
    nat_probe_finish(p, false);
    return;
  }

  uint16_t port_a = p->phase1_tests[0].mapped_port;
  uint16_t port_b = p->phase1_tests[1].mapped_port;

  XDEBUGL0("[nat-probe] phase1: port_a=%u port_b=%u", (unsigned)port_a,
           (unsigned)port_b);

  if (port_a == port_b) {
    /* Cone NAT — done. */
    XDEBUGL0("[nat-probe] phase1 result: Cone (ports match)");

    /* Close phase 1 socket before finishing. */
    for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
      if (p->phase1_tests[i].sock) {
        xSocketDestroy(p->loop, p->phase1_tests[i].sock);
        p->phase1_tests[i].sock = NULL;
        p->phase1_tests[i].fd   = -1;
      }
    }

    nat_probe_finish(p, false);
    return;
  }

  /* Symmetric NAT — proceed to Phase 2. */
  XDEBUGL0("[nat-probe] phase1 result: Symmetric (ports differ), "
           "starting phase2");

  /* Close phase 1 socket. */
  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    if (p->phase1_tests[i].sock) {
      xSocketDestroy(p->loop, p->phase1_tests[i].sock);
      p->phase1_tests[i].sock = NULL;
      p->phase1_tests[i].fd   = -1;
    }
  }

  if (!nat_probe_start_phase2(p)) {
    nat_probe_finish(p, false);
  }
}

/* ───────────────────── Check Phase 2 completion ───────────────────── */

static void nat_probe_check_phase2_done(xNatProbe_ *p) {
  if (p->phase2_done_count < XNAT_PHASE2_COUNT) return;
  nat_probe_finish(p, false);
}

/* ───────────────────── Classify NAT type ───────────────────── */

static xNatType nat_classify_phase2(const uint16_t ports[XNAT_PHASE2_COUNT],
                                    int *out_delta) {
  *out_delta = 0;

  int d1 = (int)ports[1] - (int)ports[0];
  int d2 = (int)ports[2] - (int)ports[1];

  if (d1 == d2 && d1 != 0) {
    *out_delta = d1;
    return xNatType_SymmetricSequential;
  }

  return xNatType_SymmetricRandom;
}

/* ───────────────────── Finish probe ───────────────────── */

static void nat_probe_finish(xNatProbe_ *p, bool timed_out) {
  if (p->finished) return;
  p->finished = true;

  /* Cancel overall timeout timer if still pending. */
  if (p->timeout_timer) {
    xEventLoopTimerCancel(p->loop, p->timeout_timer);
    p->timeout_timer = NULL;
  }

  xNatProbeResult result;
  memset(&result, 0, sizeof(result));

  /* Collect phase 1 mapped ports. */
  result.mapped_ports[0] = p->phase1_tests[0].mapped_port;
  result.mapped_ports[1] = p->phase1_tests[1].mapped_port;

  int phase1_success = 0;
  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    if (p->phase1_tests[i].done) phase1_success++;
  }

  if (timed_out || phase1_success < XNAT_PHASE1_COUNT) {
    XDEBUGL0("[nat-probe] completed with %d/%d phase1 responses — "
             "type=Unknown",
             phase1_success, XNAT_PHASE1_COUNT);
    result.type       = xNatType_Unknown;
    result.port_delta = 0;
    p->cb(&result, p->arg);
    nat_probe_free(p);
    return;
  }

  /* Phase 1 succeeded. */
  if (p->phase1_tests[0].mapped_port == p->phase1_tests[1].mapped_port) {
    /* Cone NAT. */
    result.type       = xNatType_Cone;
    result.port_delta = 0;
    XDEBUGL0("[nat-probe] result: type=%s ports=[%u,%u]",
             xNatTypeStr(result.type), result.mapped_ports[0],
             result.mapped_ports[1]);
    p->cb(&result, p->arg);
    nat_probe_free(p);
    return;
  }

  /* Symmetric — check phase 2 results. */
  int phase2_success = 0;
  for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
    result.mapped_ports[2 + i] = p->phase2_tests[i].mapped_port;
    if (p->phase2_tests[i].done) phase2_success++;
  }

  if (phase2_success < XNAT_PHASE2_COUNT) {
    XDEBUGL0("[nat-probe] phase2 incomplete (%d/%d) — "
             "type=SymmetricRandom (fallback)",
             phase2_success, XNAT_PHASE2_COUNT);
    result.type       = xNatType_SymmetricRandom;
    result.port_delta = 0;
  } else {
    uint16_t p2_ports[XNAT_PHASE2_COUNT];
    for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
      p2_ports[i] = p->phase2_tests[i].mapped_port;
    }
    result.type = nat_classify_phase2(p2_ports, &result.port_delta);
  }

  XDEBUGL0("[nat-probe] result: type=%s phase1=[%u,%u] phase2=[%u,%u,%u] "
           "delta=%d",
           xNatTypeStr(result.type), result.mapped_ports[0],
           result.mapped_ports[1], result.mapped_ports[2],
           result.mapped_ports[3], result.mapped_ports[4],
           result.port_delta);

  p->cb(&result, p->arg);
  nat_probe_free(p);
}

/* ───────────────────── Free ───────────────────── */

static void nat_probe_free(xNatProbe_ *p) {
  if (!p) return;

  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    xNatProbeTest *t = &p->phase1_tests[i];
    if (t->sock) {
      xSocketDestroy(p->loop, t->sock);
      t->sock = NULL;
      t->fd   = -1;
    } else if (t->fd >= 0) {
      close(t->fd);
      t->fd = -1;
    }
  }

  for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
    xNatProbeTest *t = &p->phase2_tests[i];
    if (t->sock) {
      xSocketDestroy(p->loop, t->sock);
      t->sock = NULL;
      t->fd   = -1;
    } else if (t->fd >= 0) {
      close(t->fd);
      t->fd = -1;
    }
  }

  free(p->stun_host2);
  free(p);
}

/* ───────────────────── DNS callback for STUN server 1 ───────────────────── */

static void nat_probe_on_dns1(xDnsResult *result, void *arg);
static void nat_probe_on_dns2(xDnsResult *result, void *arg);

static void nat_probe_on_dns1(xDnsResult *result, void *arg) {
  xNatProbe_ *p = (xNatProbe_ *)arg;
  p->dns_query      = NULL;

  if (p->finished) {
    xDnsResultFree(result);
    return;
  }

  if (result->error != xErrno_Ok || !result->addrs) {
    XDEBUGL0("[nat-probe] DNS resolution for server1 failed (err=%d)",
             result->error);
    xDnsResultFree(result);
    nat_probe_finish(p, false);
    return;
  }

  /* Copy the first resolved address and inject the STUN port. */
  memcpy(&p->stun_addr1, &result->addrs->addr, result->addrs->addrlen);
  if (p->stun_addr1.ss_family == AF_INET) {
    ((struct sockaddr_in *)&p->stun_addr1)->sin_port = htons(p->stun_port1);
  } else {
    ((struct sockaddr_in6 *)&p->stun_addr1)->sin6_port = htons(p->stun_port1);
  }
  xDnsResultFree(result);

  XDEBUGL1("[nat-probe] DNS1 resolved, resolving server2...");

  /* Now resolve STUN server 2. */
  p->phase = xNatProbePhase_DNS2;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = p->stun_addr1.ss_family; /* Match family of server 1. */
  hints.ai_socktype = SOCK_DGRAM;

  p->dns_query =
    xDnsResolve(p->loop, p->stun_host2, NULL, &hints, nat_probe_on_dns2, p);
  if (!p->dns_query) {
    XDEBUGL0("[nat-probe] xDnsResolve failed for server2");
    nat_probe_finish(p, false);
  }
}

static void nat_probe_on_dns2(xDnsResult *result, void *arg) {
  xNatProbe_ *p = (xNatProbe_ *)arg;
  p->dns_query      = NULL;

  if (p->finished) {
    xDnsResultFree(result);
    return;
  }

  if (result->error != xErrno_Ok || !result->addrs) {
    XDEBUGL0("[nat-probe] DNS resolution for server2 failed (err=%d)",
             result->error);
    xDnsResultFree(result);
    nat_probe_finish(p, false);
    return;
  }

  /* Copy the first resolved address and inject the STUN port. */
  memcpy(&p->stun_addr2, &result->addrs->addr, result->addrs->addrlen);
  if (p->stun_addr2.ss_family == AF_INET) {
    ((struct sockaddr_in *)&p->stun_addr2)->sin_port = htons(p->stun_port2);
  } else {
    ((struct sockaddr_in6 *)&p->stun_addr2)->sin6_port = htons(p->stun_port2);
  }
  xDnsResultFree(result);

  XDEBUGL1("[nat-probe] DNS2 resolved, starting phase1");

  if (!nat_probe_start_phase1(p)) {
    nat_probe_finish(p, false);
  }
}

/* ───────────────────── Start Phase 1 ───────────────────── */

static bool nat_probe_start_phase1(xNatProbe_ *p) {
  p->phase = xNatProbePhase_Phase1;

  int family = p->stun_addr1.ss_family;

  /*
   * Phase 1 uses ONE underlying UDP socket shared by both tests.
   * We create it via xSocketCreate (which handles O_NONBLOCK + FD_CLOEXEC),
   * then retrieve the fd via xSocketFd() for sendto/recvfrom.
   *
   * test[0] owns the xSocket; test[1] shares the same fd but has no
   * xSocket — it piggybacks on test[0]'s readable callback, dispatched
   * by matching the transaction ID.
   */

  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    p->phase1_tests[i].probe = p;
    nat_probe_gen_txn_id(p->phase1_tests[i].txn_id);
  }

  p->phase1_tests[0].sock =
    xSocketCreate(p->loop, family, SOCK_DGRAM, 0, xEvent_Read,
                  nat_probe_phase1_on_readable, &p->phase1_tests[0]);
  if (!p->phase1_tests[0].sock) {
    XDEBUGL0("[nat-probe] phase1: xSocketCreate failed");
    return false;
  }

  int fd = xSocketFd(p->phase1_tests[0].sock);
  p->phase1_fd = fd;

  /* Both tests share this fd. */
  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    p->phase1_tests[i].fd = fd;
  }

  xSocketSetTimeout(p->phase1_tests[0].sock, p->timeout_ms, 0);

  /* Send Binding Requests to both STUN servers from the same socket. */
  if (!nat_probe_send_request_to(p, &p->phase1_tests[0], fd, &p->stun_addr1)) {
    p->phase1_tests[0].failed = true;
    p->phase1_done_count++;
  }
  if (!nat_probe_send_request_to(p, &p->phase1_tests[1], fd, &p->stun_addr2)) {
    p->phase1_tests[1].failed = true;
    p->phase1_done_count++;
  }

  nat_probe_check_phase1_done(p);
  return true;
}

/* ───────────────────── Phase 1 read callback (shared socket) ───────────────────── */

/*
 * Phase 1 readable callback: since both phase1 tests share one fd,
 * this callback dispatches responses to the correct test by matching
 * the transaction ID.
 */
static void nat_probe_phase1_on_readable(xSocket sock, xEventMask mask,
                                         void *arg) {
  /* arg is always &phase1_tests[0] since that's the xSocket owner. */
  xNatProbeTest  *t0 = (xNatProbeTest *)arg;
  xNatProbe_ *p  = t0->probe;

  (void)sock;

  if (p->finished) return;

  if (mask & xEvent_Timeout) {
    XDEBUGL1("[nat-probe] phase1 socket timed out");
    for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
      if (!p->phase1_tests[i].done && !p->phase1_tests[i].failed) {
        p->phase1_tests[i].failed = true;
        p->phase1_done_count++;
      }
    }
    nat_probe_check_phase1_done(p);
    return;
  }

  /* Read all available datagrams. */
  for (;;) {
    uint8_t                 buf[XSTUN_MAX_MSG_SIZE];
    struct sockaddr_storage from;
    socklen_t               from_len = sizeof(from);

    ssize_t n = recvfrom(t0->fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n <= 0) break;

    /* Try to match against each phase1 test's txn_id. */
    for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
      xNatProbeTest *t = &p->phase1_tests[i];
      if (t->done || t->failed) continue;

      uint16_t port;
      if (nat_probe_parse_response(buf, (size_t)n, t->txn_id, &port)) {
        t->mapped_port = port;
        t->done        = true;
        p->phase1_done_count++;
        XDEBUGL1("[nat-probe] phase1 test[%d] mapped port = %u", i,
                 (unsigned)port);
        break; /* Each response matches at most one test. */
      }
    }
  }

  nat_probe_check_phase1_done(p);
}

/* ───────────────────── Start Phase 2 ───────────────────── */

static bool nat_probe_start_phase2(xNatProbe_ *p) {
  p->phase = xNatProbePhase_Phase2;

  int family = p->stun_addr1.ss_family;

  for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
    xNatProbeTest *t = &p->phase2_tests[i];
    t->probe = p;

    nat_probe_gen_txn_id(t->txn_id);

    t->sock = xSocketCreate(p->loop, family, SOCK_DGRAM, 0, xEvent_Read,
                            nat_probe_phase2_on_readable, t);
    if (!t->sock) {
      XDEBUGL0("[nat-probe] phase2: xSocketCreate failed for test[%d]", i);
      return false;
    }
    t->fd = xSocketFd(t->sock);

    xSocketSetTimeout(t->sock, p->timeout_ms, 0);
  }

  /* Send STUN Binding Requests to server A from each socket. */
  for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
    if (!nat_probe_send_request_to(p, &p->phase2_tests[i],
                                   p->phase2_tests[i].fd, &p->stun_addr1)) {
      p->phase2_tests[i].failed = true;
      p->phase2_done_count++;
    }
  }

  nat_probe_check_phase2_done(p);
  return true;
}

/* ───────────────────── Send one STUN Binding Request ───────────────────── */

static bool nat_probe_send_request_to(xNatProbe_ *p, xNatProbeTest *t,
                                      int fd,
                                      struct sockaddr_storage *addr) {
  (void)p;

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, t->txn_id);

  uint8_t buf[XSTUN_MAX_MSG_SIZE];
  int     len = xStunMsgEncode(&msg, buf, sizeof(buf));
  if (len < 0) {
    XDEBUGL1("[nat-probe] encode failed");
    return false;
  }

  socklen_t addrlen = addr->ss_family == AF_INET6
                        ? sizeof(struct sockaddr_in6)
                        : sizeof(struct sockaddr_in);

  ssize_t n = sendto(fd, buf, (size_t)len, 0,
                     (const struct sockaddr *)addr, addrlen);
  if (n < 0) {
    XDEBUGL1("[nat-probe] sendto failed: %s", strerror(errno));
    return false;
  }

  XDEBUGL1("[nat-probe] sent Binding Request (fd=%d)", fd);
  return true;
}

/* ───────────────────── Generate random transaction ID ───────────────────── */

static void nat_probe_gen_txn_id(uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(txn_id, XSTUN_TXN_ID_SIZE);
#else
  for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++) {
    txn_id[i] = (uint8_t)(rand() & 0xFF);
  }
#endif
}

/* ───────────────────── Public API ───────────────────── */

XCAPI(xNatProbe) xNatProbeStart(xEventLoop loop,
                                const char *stun_host1, uint16_t stun_port1,
                                const char *stun_host2, uint16_t stun_port2,
                                int timeout_ms,
                                xNatProbeFunc cb, void *arg) {
  if (!loop || !stun_host1 || !stun_host2 || !cb) return NULL;

  if (timeout_ms <= 0) timeout_ms = XNAT_PROBE_DEFAULT_TIMEOUT_MS;

  xNatProbe_ *p = (xNatProbe_ *)calloc(1, sizeof(*p));
  if (!p) return NULL;

  p->loop       = loop;
  p->cb         = cb;
  p->arg        = arg;
  p->stun_port1 = stun_port1;
  p->stun_port2 = stun_port2;
  p->timeout_ms = timeout_ms;
  p->finished   = false;
  p->phase1_fd  = -1;

  p->stun_host2 = strdup(stun_host2);
  if (!p->stun_host2) {
    free(p);
    return NULL;
  }

  for (int i = 0; i < XNAT_PHASE1_COUNT; i++) {
    p->phase1_tests[i].fd    = -1;
    p->phase1_tests[i].probe = p;
  }
  for (int i = 0; i < XNAT_PHASE2_COUNT; i++) {
    p->phase2_tests[i].fd    = -1;
    p->phase2_tests[i].probe = p;
  }

  /* Schedule overall timeout — covers DNS + Phase1 + Phase2. */
  int total_timeout = timeout_ms * 2 + 2000; /* generous for two phases */
  p->timeout_timer = xEventLoopTimerAfter(p->loop, nat_probe_on_timeout, p,
                                          (uint64_t)total_timeout);

  /* Resolve STUN server 1 first. */
  p->phase = xNatProbePhase_DNS1;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  p->dns_query =
    xDnsResolve(loop, stun_host1, NULL, &hints, nat_probe_on_dns1, p);
  if (!p->dns_query) {
    XDEBUGL0("[nat-probe] xDnsResolve failed for %s", stun_host1);
    xEventLoopTimerCancel(loop, p->timeout_timer);
    free(p->stun_host2);
    free(p);
    return NULL;
  }

  XDEBUGL0("[nat-probe] resolving %s:%u + %s:%u (timeout=%dms)", stun_host1,
           (unsigned)stun_port1, stun_host2, (unsigned)stun_port2, timeout_ms);

  return (xNatProbe)p;
}

XCAPI(void) xNatProbeCancel(xNatProbe probe) {
  if (!probe) return;
  xNatProbe_ *p = (xNatProbe_ *)probe;
  if (p->finished) return;
  p->finished = true;

  if (p->dns_query) {
    xDnsCancel(p->loop, p->dns_query);
    p->dns_query = NULL;
  }

  if (p->timeout_timer) {
    xEventLoopTimerCancel(p->loop, p->timeout_timer);
    p->timeout_timer = NULL;
  }

  nat_probe_free(p);
}

XCAPI(const char *) xNatTypeStr(xNatType type) {
  switch (type) {
  case xNatType_Unknown:
    return "Unknown";
  case xNatType_OpenInternet:
    return "OpenInternet";
  case xNatType_Cone:
    return "Cone";
  case xNatType_SymmetricRandom:
    return "SymmetricRandom";
  case xNatType_SymmetricSequential:
    return "SymmetricSequential";
  default:
    return "Unknown";
  }
}
