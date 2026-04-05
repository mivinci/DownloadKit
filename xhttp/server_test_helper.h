/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_test_helper.h - Shared helpers for xhttp server tests
 */

#ifndef XHTTP_SERVER_TEST_HELPER_H_
#define XHTTP_SERVER_TEST_HELPER_H_

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>

extern "C" {
#include <xhttp/server.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* ───────────────────── Helpers ───────────────────── */

/**
 * @brief Pump the event loop for a given number of milliseconds.
 */
static inline void pump_loop(xEventLoop loop, int ms) {
  for (int elapsed = 0; elapsed < ms; elapsed += 5) {
    xEventWait(loop, 5);
  }
}

/**
 * @brief Find a free port by binding to port 0.
 */
static inline uint16_t find_free_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return 0;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
    close(fd);
    return 0;
  }

  uint16_t port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

/**
 * @brief Connect to localhost on the given port and return the fd.
 */
static inline int connect_to(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = htons(port);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/**
 * @brief Send a string over a socket.
 */
static inline bool send_str(int fd, const std::string &s) {
  ssize_t n = send(fd, s.data(), s.size(), 0);
  return n == (ssize_t)s.size();
}

/**
 * @brief Receive all available data from a socket (with timeout).
 */
static inline std::string recv_all(int fd, int timeout_ms = 2000) {
  std::string result;
  char buf[4096];

  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    result.append(buf, (size_t)n);

    /* If we got a complete HTTP response, stop */
    if (result.find("\r\n\r\n") != std::string::npos) {
      /* Check if we have Content-Length and have received the full body */
      auto cl_pos = result.find("Content-Length: ");
      if (cl_pos != std::string::npos) {
        size_t cl_start = cl_pos + 16;
        size_t cl_end   = result.find("\r\n", cl_start);
        if (cl_end != std::string::npos) {
          int content_len = std::stoi(result.substr(cl_start, cl_end - cl_start));
          size_t body_start = result.find("\r\n\r\n") + 4;
          if (result.size() >= body_start + (size_t)content_len) break;
        }
      } else {
        break;
      }
    }
  }
  return result;
}

/* ───────────────────── Shared context structs ───────────────────── */

struct HandlerCtx {
  std::atomic<int> call_count{0};
  std::string      last_method;
  std::string      last_url;
  std::string      last_body;
};

struct ParamHandlerCtx {
  std::atomic<int> call_count{0};
  std::string      param_id;
  std::string      param_action;
};

/* ───────────────────── Fixture ───────────────────── */

class HttpServerTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpServer server = nullptr;
  uint16_t    port   = 0;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    server = xHttpServerCreate(loop);
    ASSERT_NE(server, nullptr);

    port = find_free_port();
    ASSERT_NE(port, 0) << "Could not find a free port";
  }

  void TearDown() override {
    if (server) xHttpServerDestroy(server);
    if (loop)   xEventLoopDestroy(loop);
  }

  /** Start listening and pump briefly to let the socket settle. */
  void listen_and_pump() {
    xErrno err = xHttpServerListen(server, "127.0.0.1", port);
    ASSERT_EQ(err, xErrno_Ok) << "Failed to listen on port " << port;
    pump_loop(loop, 20);
  }
};

#endif /* XHTTP_SERVER_TEST_HELPER_H_ */
