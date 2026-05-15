/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_echo_server.cpp - TCP echo server for end-to-end benchmarking
 *
 * Usage: tcp_echo_server [port]
 *   Default port: 9000
 *
 * Echoes back every byte received on each connection.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>

extern "C" {
#include <xbase/event.h>
#include <xbase/socket.h>
#include <xnet/tcp.h>
}

static xEventLoop g_loop = nullptr;

static void on_readable(int fd, xEventMask mask, void *arg) {
  (void)fd;
  (void)mask;
  xTcpConn conn = static_cast<xTcpConn>(arg);
  char buf[65536];

  for (;;) {
    ssize_t n = xTcpConnRecv(conn, buf, sizeof(buf));
    if (n > 0) {
      // Echo back
      ssize_t sent = 0;
      while (sent < n) {
        ssize_t w = xTcpConnSend(conn, buf + sent, n - sent);
        if (w <= 0) break;
        sent += w;
      }
    } else if (n == 0) {
      // EOF
      xTcpConnClose(g_loop, conn);
      return;
    } else {
      // EAGAIN or error
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      xTcpConnClose(g_loop, conn);
      return;
    }
  }
}

static void on_accept(xTcpListener listener, xTcpConn conn,
                      const struct sockaddr *addr, socklen_t addrlen,
                      void *arg) {
  (void)listener;
  (void)addr;
  (void)addrlen;
  (void)arg;

  xSocket sock = xTcpConnSocket(conn);
  int fd = xSocketFd(sock);

  xEventAdd(g_loop, fd, xEvent_Read, on_readable, conn);
}

int main(int argc, char *argv[]) {
  uint16_t port = 9000;
  if (argc > 1) port = (uint16_t)atoi(argv[1]);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  // Watch SIGINT to stop gracefully
  xEventLoopSignalWatch(g_loop, SIGINT,
                        [](int, void *) { xEventLoopStop(g_loop); }, nullptr);

  xTcpListenerConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.reuseport = 1;

  xTcpListener listener =
      xTcpListenerCreate(g_loop, "0.0.0.0", port, &conf, on_accept, nullptr);
  if (!listener) {
    fprintf(stderr, "Failed to listen on port %u\n", port);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "TCP echo server listening on 0.0.0.0:%u\n", port);
  fflush(stdout);

  xEventLoopRun(g_loop);

  xTcpListenerDestroy(listener);
  xEventLoopDestroy(g_loop);
  return 0;
}
