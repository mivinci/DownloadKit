/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nat_probe_test.cpp - Unit tests for NAT type detection
 */

#include <gtest/gtest.h>

extern "C" {
#include "nat_probe.h"
#include "stun_attr.h"
#include "stun_msg.h"
}

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

/**
 * @brief Build a STUN Binding Success Response with a XOR-MAPPED-ADDRESS
 *        attribute containing the given mapped port.
 *
 * @param txn_id   Transaction ID from the request (12 bytes).
 * @param port     The mapped port to encode.
 * @param buf      Output buffer.
 * @param buf_size Size of output buffer.
 * @return         Encoded length, or -1 on error.
 */
static int build_stun_response(const uint8_t txn_id[XSTUN_TXN_ID_SIZE],
                               uint16_t port, uint8_t *buf, size_t buf_size) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingResponse, txn_id);

  /* Build XOR-MAPPED-ADDRESS attribute in a temp buffer. */
  uint8_t         attr_buf[64];
  xStunAttrWriter writer;
  xStunAttrWriterInit(&writer, attr_buf, sizeof(attr_buf));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  inet_pton(AF_INET, "203.0.113.1", &addr.sin_addr);

  if (xStunAttrWriteXorMappedAddress(&writer, (struct sockaddr *)&addr,
                                     txn_id) != xErrno_Ok) {
    return -1;
  }

  msg.attrs     = attr_buf;
  msg.attrs_len = (uint16_t)writer.pos;

  return xStunMsgEncode(&msg, buf, buf_size);
}

/**
 * @brief A minimal mock STUN server that listens on a local UDP socket
 *        and replies to Binding Requests with a configurable mapped port.
 *
 * For Phase 1 tests, the port is fixed.
 * For Phase 2 tests, the port increments per request (to simulate
 * different mapped ports for different source sockets).
 */
class MockStunServer {
public:
  /**
   * @param base_port  The mapped port to return in the first response.
   * @param increment  Port increment per subsequent request (0 = fixed).
   */
  MockStunServer(uint16_t base_port, int increment = 0)
      : base_port_(base_port), increment_(increment), fd_(-1),
        wake_rfd_(-1), wake_wfd_(-1), running_(false), request_count_(0) {}

  ~MockStunServer() { Stop(); }

  /** Start listening. Returns the local port the server is bound to. */
  uint16_t Start() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* OS picks a free port. */

    if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      close(fd_);
      fd_ = -1;
      return 0;
    }

    /* Retrieve the assigned port. */
    socklen_t len = sizeof(addr);
    getsockname(fd_, (struct sockaddr *)&addr, &len);
    local_port_ = ntohs(addr.sin_port);

    /* Self-pipe used to reliably wake up the worker thread in Stop().
     * close(fd) alone is NOT guaranteed to unblock a recvfrom() on Linux,
     * and shutdown() on an unconnected UDP socket returns ENOTCONN without
     * waking any blocked syscall, so poll() over (fd_, wake_rfd_) is the
     * only portable pattern. */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
      close(fd_);
      fd_ = -1;
      return 0;
    }
    wake_rfd_ = pipefd[0];
    wake_wfd_ = pipefd[1];

    running_ = true;
    thread_  = std::thread([this]() { Run(); });
    return local_port_;
  }

  void Stop() {
    if (!running_.exchange(false)) {
      /* Never started (or already stopped) — still clean up fds. */
    } else if (wake_wfd_ >= 0) {
      /* Wake the worker out of poll(). Errors are intentionally ignored. */
      const uint8_t byte = 0;
      ssize_t       n    = write(wake_wfd_, &byte, 1);
      (void)n;
    }
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (wake_rfd_ >= 0) {
      close(wake_rfd_);
      wake_rfd_ = -1;
    }
    if (wake_wfd_ >= 0) {
      close(wake_wfd_);
      wake_wfd_ = -1;
    }
  }

  int RequestCount() const { return request_count_.load(); }

private:
  void Run() {
    uint8_t                 buf[XSTUN_MAX_MSG_SIZE];
    struct sockaddr_storage from;
    socklen_t               from_len;

    while (running_) {
      struct pollfd pfds[2];
      pfds[0].fd      = fd_;
      pfds[0].events  = POLLIN;
      pfds[0].revents = 0;
      pfds[1].fd      = wake_rfd_;
      pfds[1].events  = POLLIN;
      pfds[1].revents = 0;

      int pr = poll(pfds, 2, -1);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) break;
      if (!(pfds[0].revents & POLLIN)) continue;

      from_len = sizeof(from);
      ssize_t n =
        recvfrom(fd_, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        break;
      }

      if (!xStunMsgIsStun(buf, (size_t)n)) continue;

      xStunMsg req;
      if (xStunMsgDecode(&req, buf, (size_t)n) != xErrno_Ok) continue;
      if (!xStunMsgIsRequest(req.type)) continue;

      int      idx  = request_count_.fetch_add(1);
      uint16_t port = (uint16_t)(base_port_ + idx * increment_);

      uint8_t resp_buf[XSTUN_MAX_MSG_SIZE];
      int     resp_len =
        build_stun_response(req.txn_id, port, resp_buf, sizeof(resp_buf));
      if (resp_len < 0) continue;

      sendto(fd_, resp_buf, (size_t)resp_len, 0, (struct sockaddr *)&from,
             from_len);
    }
  }

  uint16_t             base_port_;
  int                  increment_;
  int                  fd_;
  int                  wake_rfd_;
  int                  wake_wfd_;
  uint16_t             local_port_ = 0;
  std::atomic<bool>    running_;
  std::atomic<int>     request_count_;
  std::thread          thread_;
};

/**
 * @brief A mock STUN server that never responds (for timeout tests).
 */
class SilentStunServer {
public:
  SilentStunServer()
      : fd_(-1), wake_rfd_(-1), wake_wfd_(-1), running_(false) {}
  ~SilentStunServer() { Stop(); }

  uint16_t Start() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      close(fd_);
      fd_ = -1;
      return 0;
    }

    socklen_t len = sizeof(addr);
    getsockname(fd_, (struct sockaddr *)&addr, &len);
    local_port_ = ntohs(addr.sin_port);

    /* See MockStunServer::Start() for why we need a self-pipe. */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
      close(fd_);
      fd_ = -1;
      return 0;
    }
    wake_rfd_ = pipefd[0];
    wake_wfd_ = pipefd[1];

    running_ = true;
    thread_  = std::thread([this]() {
      /* Sleep until Stop() writes to the wake pipe. Any datagrams that do
       * arrive on fd_ are silently dropped, which is the point of this
       * server: never answer. */
      uint8_t       buf[64];
      struct pollfd pfds[2];
      while (running_) {
        pfds[0].fd      = fd_;
        pfds[0].events  = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd      = wake_rfd_;
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if (pfds[0].revents & POLLIN) {
          /* Drain the datagram so the socket buffer does not fill up. */
          (void)recvfrom(fd_, buf, sizeof(buf), 0, nullptr, nullptr);
        }
      }
    });
    return local_port_;
  }

  void Stop() {
    if (!running_.exchange(false)) {
      /* Never started (or already stopped). */
    } else if (wake_wfd_ >= 0) {
      const uint8_t byte = 0;
      ssize_t       n    = write(wake_wfd_, &byte, 1);
      (void)n;
    }
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (wake_rfd_ >= 0) {
      close(wake_rfd_);
      wake_rfd_ = -1;
    }
    if (wake_wfd_ >= 0) {
      close(wake_wfd_);
      wake_wfd_ = -1;
    }
  }

private:
  int               fd_;
  int               wake_rfd_;
  int               wake_wfd_;
  uint16_t          local_port_ = 0;
  std::atomic<bool> running_;
  std::thread       thread_;
};

/* ───────────────────── Test context for async callback ───────────────────── */

struct ProbeCtx {
  std::atomic<bool> done{false};
  xNatProbeResult   result{};
};

static void probe_callback(const xNatProbeResult *result, void *arg) {
  auto *ctx    = static_cast<ProbeCtx *>(arg);
  ctx->result  = *result;
  ctx->done    = true;
}

/** Run the event loop until the probe completes or timeout. */
static bool wait_for_probe(xEventLoop loop, ProbeCtx &ctx, int max_ms = 8000) {
  auto start = std::chrono::steady_clock::now();
  while (!ctx.done) {
    xEventWait(loop, 50);
    auto elapsed = std::chrono::duration_cast<ms>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    if (elapsed > max_ms) return false;
  }
  return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  xNatTypeStr tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeTypeStr, AllKnownTypes) {
  EXPECT_STREQ(xNatTypeStr(xNatType_Unknown), "Unknown");
  EXPECT_STREQ(xNatTypeStr(xNatType_OpenInternet), "OpenInternet");
  EXPECT_STREQ(xNatTypeStr(xNatType_Cone), "Cone");
  EXPECT_STREQ(xNatTypeStr(xNatType_SymmetricRandom), "SymmetricRandom");
  EXPECT_STREQ(xNatTypeStr(xNatType_SymmetricSequential),
               "SymmetricSequential");
}

TEST(NatProbeTypeStr, InvalidType) {
  EXPECT_STREQ(xNatTypeStr((xNatType)99), "Unknown");
  EXPECT_STREQ(xNatTypeStr((xNatType)-1), "Unknown");
}

/* ═══════════════════════════════════════════════════════════════════
 *  API parameter validation
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeAPI, NullLoopReturnsNull) {
  ProbeCtx ctx;
  EXPECT_EQ(xNatProbeStart(nullptr, "stun.l.google.com", 3478,
                           "stun1.l.google.com", 3478, 1000, probe_callback,
                           &ctx),
            nullptr);
}

TEST(NatProbeAPI, NullHost1ReturnsNull) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  ProbeCtx ctx;
  EXPECT_EQ(
    xNatProbeStart(loop, nullptr, 3478, "stun1.l.google.com", 3478, 1000,
                   probe_callback, &ctx),
    nullptr);
  xEventLoopDestroy(loop);
}

TEST(NatProbeAPI, NullHost2ReturnsNull) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  ProbeCtx ctx;
  EXPECT_EQ(xNatProbeStart(loop, "stun.l.google.com", 3478, nullptr, 3478,
                           1000, probe_callback, &ctx),
            nullptr);
  xEventLoopDestroy(loop);
}

TEST(NatProbeAPI, NullCallbackReturnsNull) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  EXPECT_EQ(xNatProbeStart(loop, "stun.l.google.com", 3478,
                           "stun1.l.google.com", 3478, 1000, nullptr, nullptr),
            nullptr);
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Cancel safety
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeCancel, CancelNullDoesNotCrash) {
  xNatProbeCancel(nullptr);
}

TEST(NatProbeCancel, CancelImmediatelyAfterStart) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  /* Use a real hostname — the probe will start DNS resolution. */
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", 3478, "127.0.0.1", 3479, 5000,
                   probe_callback, &ctx);
  /* Cancel immediately — should not crash. */
  xNatProbeCancel(probe);

  /* Callback should NOT have been invoked. */
  EXPECT_FALSE(ctx.done);

  /* Pump the event loop briefly to let any in-flight thread pool tasks
     drain (DNS offload may still be running). */
  for (int i = 0; i < 20; i++)
    xEventWait(loop, 10);

  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Cone NAT
 *
 *  Both mock STUN servers return the same mapped port → Cone.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, ConeNat) {
  /* Both servers return mapped port 5000. */
  MockStunServer server1(5000, 0);
  MockStunServer server2(5000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_Cone);
  EXPECT_EQ(ctx.result.port_delta, 0);
  EXPECT_EQ(ctx.result.mapped_ports[0], 5000);
  EXPECT_EQ(ctx.result.mapped_ports[1], 5000);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Symmetric Sequential NAT
 *
 *  Phase 1: servers return different ports → Symmetric.
 *  Phase 2: server1 returns ports with constant delta → Sequential.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, SymmetricSequential) {
  /*
   * Server1 handles both Phase 1 (1 request) and Phase 2 (3 requests).
   * Phase 1 request → port 5000 (idx=0)
   * Phase 2 requests → port 5002 (idx=1), 5004 (idx=2), 5006 (idx=3)
   * Deltas: 5004-5002=2, 5006-5004=2 → Sequential with delta=2.
   */
  MockStunServer server1(5000, 2);
  /* Server2 returns a different port for Phase 1. */
  MockStunServer server2(6000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_SymmetricSequential);
  EXPECT_EQ(ctx.result.port_delta, 2);

  /* Phase 1: ports differ. */
  EXPECT_EQ(ctx.result.mapped_ports[0], 5000); /* server1, idx=0 */
  EXPECT_EQ(ctx.result.mapped_ports[1], 6000); /* server2, idx=0 */

  /* Phase 2: sequential ports from server1 (idx=1,2,3). */
  EXPECT_EQ(ctx.result.mapped_ports[2], 5002);
  EXPECT_EQ(ctx.result.mapped_ports[3], 5004);
  EXPECT_EQ(ctx.result.mapped_ports[4], 5006);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Symmetric Random NAT
 *
 *  Phase 1: servers return different ports → Symmetric.
 *  Phase 2: server1 returns ports with varying deltas → Random.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, SymmetricRandom) {
  /*
   * We need Phase 2 to produce non-uniform deltas.
   * Server1 with increment=0 returns the same port for all requests.
   * That means Phase 2 ports are all the same → d1=d2=0 → classified
   * as SymmetricRandom (because d1==0 is excluded from Sequential).
   *
   * Actually, let's use a custom approach: we'll use two separate
   * servers for Phase 1 (different ports), and for Phase 2 we need
   * server1 to return ports with non-uniform deltas.
   *
   * With MockStunServer(base=5000, increment=0):
   *   Phase 1: idx=0 → 5000
   *   Phase 2: idx=1 → 5000, idx=2 → 5000, idx=3 → 5000
   *   d1 = 0, d2 = 0, d1==d2 but d1==0 → SymmetricRandom ✓
   */
  MockStunServer server1(5000, 0); /* All responses return port 5000. */
  MockStunServer server2(6000, 0); /* Phase 1 returns port 6000. */

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_SymmetricRandom);
  EXPECT_EQ(ctx.result.port_delta, 0);

  /* Phase 1: ports differ. */
  EXPECT_EQ(ctx.result.mapped_ports[0], 5000);
  EXPECT_EQ(ctx.result.mapped_ports[1], 6000);

  /* Phase 2: all same port. */
  EXPECT_EQ(ctx.result.mapped_ports[2], 5000);
  EXPECT_EQ(ctx.result.mapped_ports[3], 5000);
  EXPECT_EQ(ctx.result.mapped_ports[4], 5000);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Symmetric Random with non-uniform deltas
 *
 *  Phase 2 ports have genuinely different deltas.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief A mock STUN server that returns ports from a predefined list.
 */
class ScriptedStunServer {
public:
  ScriptedStunServer(std::vector<uint16_t> ports)
      : ports_(std::move(ports)), fd_(-1), running_(false),
        request_count_(0) {}

  ~ScriptedStunServer() { Stop(); }

  uint16_t Start() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      close(fd_);
      fd_ = -1;
      return 0;
    }

    socklen_t len = sizeof(addr);
    getsockname(fd_, (struct sockaddr *)&addr, &len);
    local_port_ = ntohs(addr.sin_port);

    running_ = true;
    thread_  = std::thread([this]() { Run(); });
    return local_port_;
  }

  void Stop() {
    running_ = false;
    if (fd_ >= 0) {
      shutdown(fd_, SHUT_RDWR);
      close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
  }

private:
  void Run() {
    uint8_t                 buf[XSTUN_MAX_MSG_SIZE];
    struct sockaddr_storage from;
    socklen_t               from_len;

    while (running_) {
      from_len = sizeof(from);
      ssize_t n =
        recvfrom(fd_, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
      if (n <= 0) break;

      if (!xStunMsgIsStun(buf, (size_t)n)) continue;

      xStunMsg req;
      if (xStunMsgDecode(&req, buf, (size_t)n) != xErrno_Ok) continue;
      if (!xStunMsgIsRequest(req.type)) continue;

      int      idx  = request_count_.fetch_add(1);
      uint16_t port = (idx < (int)ports_.size()) ? ports_[idx] : ports_.back();

      uint8_t resp_buf[XSTUN_MAX_MSG_SIZE];
      int     resp_len =
        build_stun_response(req.txn_id, port, resp_buf, sizeof(resp_buf));
      if (resp_len < 0) continue;

      sendto(fd_, resp_buf, (size_t)resp_len, 0, (struct sockaddr *)&from,
             from_len);
    }
  }

  std::vector<uint16_t> ports_;
  int                   fd_;
  uint16_t              local_port_ = 0;
  std::atomic<bool>     running_;
  std::atomic<int>      request_count_;
  std::thread           thread_;
};

TEST(NatProbeIntegration, SymmetricRandomNonUniformDeltas) {
  /*
   * Server1 scripted responses:
   *   Phase 1 (idx=0): port 5000
   *   Phase 2 (idx=1): port 5010
   *   Phase 2 (idx=2): port 5013  (delta=3)
   *   Phase 2 (idx=3): port 5020  (delta=7)
   * d1=3, d2=7 → non-uniform → SymmetricRandom.
   */
  ScriptedStunServer server1({5000, 5010, 5013, 5020});
  MockStunServer     server2(6000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_SymmetricRandom);
  EXPECT_EQ(ctx.result.port_delta, 0);

  /* Phase 1: ports differ. */
  EXPECT_EQ(ctx.result.mapped_ports[0], 5000);
  EXPECT_EQ(ctx.result.mapped_ports[1], 6000);

  /* Phase 2: non-uniform deltas. */
  EXPECT_EQ(ctx.result.mapped_ports[2], 5010);
  EXPECT_EQ(ctx.result.mapped_ports[3], 5013);
  EXPECT_EQ(ctx.result.mapped_ports[4], 5020);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Timeout → Unknown
 *
 *  Both servers never respond → overall timeout → Unknown.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, TimeoutReturnsUnknown) {
  SilentStunServer server1;
  SilentStunServer server2;

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  /* Use a short timeout so the test doesn't take too long. */
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 500,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  /* Total timeout = 500*2 + 2000 = 3000ms. Wait up to 5s. */
  ASSERT_TRUE(wait_for_probe(loop, ctx, 5000));

  EXPECT_EQ(ctx.result.type, xNatType_Unknown);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Phase 1 partial failure → Unknown
 *
 *  Server1 responds, server2 never responds → phase1 incomplete.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, Phase1PartialFailure) {
  MockStunServer   server1(5000, 0);
  SilentStunServer server2;

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 500,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx, 5000));

  EXPECT_EQ(ctx.result.type, xNatType_Unknown);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: DNS failure → Unknown
 *
 *  Use an unresolvable hostname.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, DnsFailure) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe = xNatProbeStart(
    loop, "this.host.does.not.exist.invalid", 3478,
    "also.does.not.exist.invalid", 3478, 1000, probe_callback, &ctx);

  if (probe) {
    /* DNS resolution is async — wait for callback. */
    ASSERT_TRUE(wait_for_probe(loop, ctx, 10000));
    EXPECT_EQ(ctx.result.type, xNatType_Unknown);
  }
  /* If probe is NULL, xDnsResolve itself failed — also acceptable. */

  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Cone with different base ports
 *
 *  Verify that Cone detection works with various port values.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, ConeWithHighPort) {
  MockStunServer server1(65000, 0);
  MockStunServer server2(65000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_Cone);
  EXPECT_EQ(ctx.result.mapped_ports[0], 65000);
  EXPECT_EQ(ctx.result.mapped_ports[1], 65000);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Symmetric Sequential with delta=1
 *
 *  The most common sequential pattern.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, SymmetricSequentialDelta1) {
  /*
   * Server1 scripted:
   *   Phase 1 (idx=0): port 10000
   *   Phase 2 (idx=1): port 10001
   *   Phase 2 (idx=2): port 10002
   *   Phase 2 (idx=3): port 10003
   * d1=1, d2=1 → Sequential with delta=1.
   */
  ScriptedStunServer server1({10000, 10001, 10002, 10003});
  MockStunServer     server2(20000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_SymmetricSequential);
  EXPECT_EQ(ctx.result.port_delta, 1);

  EXPECT_EQ(ctx.result.mapped_ports[0], 10000);
  EXPECT_EQ(ctx.result.mapped_ports[1], 20000);
  EXPECT_EQ(ctx.result.mapped_ports[2], 10001);
  EXPECT_EQ(ctx.result.mapped_ports[3], 10002);
  EXPECT_EQ(ctx.result.mapped_ports[4], 10003);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration: Symmetric Sequential with negative delta
 *
 *  Some NATs decrement ports.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, SymmetricSequentialNegativeDelta) {
  /*
   * Server1 scripted:
   *   Phase 1 (idx=0): port 10000
   *   Phase 2 (idx=1): port 9997
   *   Phase 2 (idx=2): port 9994
   *   Phase 2 (idx=3): port 9991
   * d1=-3, d2=-3 → Sequential with delta=-3.
   */
  ScriptedStunServer server1({10000, 9997, 9994, 9991});
  MockStunServer     server2(20000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 3000,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_SymmetricSequential);
  EXPECT_EQ(ctx.result.port_delta, -3);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Default timeout
 *
 *  Pass timeout_ms=0 to use the default (3000ms).
 * ═══════════════════════════════════════════════════════════════════ */

TEST(NatProbeIntegration, DefaultTimeout) {
  MockStunServer server1(5000, 0);
  MockStunServer server2(5000, 0);

  uint16_t port1 = server1.Start();
  uint16_t port2 = server2.Start();
  ASSERT_NE(port1, 0);
  ASSERT_NE(port2, 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  ProbeCtx ctx;
  /* timeout_ms = 0 → should use default. */
  xNatProbe probe =
    xNatProbeStart(loop, "127.0.0.1", port1, "127.0.0.1", port2, 0,
                   probe_callback, &ctx);
  ASSERT_NE(probe, nullptr);

  ASSERT_TRUE(wait_for_probe(loop, ctx));

  EXPECT_EQ(ctx.result.type, xNatType_Cone);

  server1.Stop();
  server2.Stop();
  xEventLoopDestroy(loop);
}
