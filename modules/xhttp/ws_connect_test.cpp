/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_connect_test.cpp - WebSocket client integration tests
 */

#include "server_test_helper.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>

extern "C" {
#include <xhttp/server.h>
#include <xhttp/ws.h>
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shared callback state
 * ═══════════════════════════════════════════════════════════════════
 */

struct WsClientCtx {
  std::atomic<int> open_count{0};
  std::atomic<int> message_count{0};
  std::atomic<int> close_count{0};
  std::string      last_message;
  xWsOpcode        last_opcode{xWsOpcode_Text};
  uint16_t         close_code{0};
  xWsConn          conn{nullptr};
};

static void client_on_open(xWsConn conn, void *arg) {
  auto *ctx = (WsClientCtx *)arg;
  ctx->open_count++;
  ctx->conn = conn;
}

static void client_on_message(xWsConn conn, xWsOpcode opcode,
                              const void *payload, size_t len, void *arg) {
  (void)conn;
  auto *ctx = (WsClientCtx *)arg;
  ctx->message_count++;
  ctx->last_opcode = opcode;
  if (payload && len > 0) {
    ctx->last_message = std::string((const char *)payload, len);
  } else {
    ctx->last_message.clear();
  }
}

static void client_on_close(xWsConn conn, uint16_t code, const char *reason,
                            size_t len, void *arg) {
  (void)conn;
  (void)reason;
  (void)len;
  auto *ctx = (WsClientCtx *)arg;
  ctx->close_count++;
  ctx->close_code = code;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Server-side callbacks (echo server)
 * ═══════════════════════════════════════════════════════════════════
 */

struct WsServerCtx {
  std::atomic<int> open_count{0};
  std::atomic<int> message_count{0};
  std::atomic<int> close_count{0};
  xWsConn          conn{nullptr};
};

static void server_on_open(xWsConn conn, void *arg) {
  auto *ctx = (WsServerCtx *)arg;
  ctx->open_count++;
  ctx->conn = conn;
}

static void server_on_message(xWsConn conn, xWsOpcode opcode,
                              const void *payload, size_t len, void *arg) {
  auto *ctx = (WsServerCtx *)arg;
  ctx->message_count++;
  /* Echo back */
  xWsSend(conn, opcode, payload, len);
}

static void server_on_close(xWsConn conn, uint16_t code, const char *reason,
                            size_t len, void *arg) {
  (void)code;
  (void)conn;
  (void)reason;
  (void)len;
  auto *ctx = (WsServerCtx *)arg;
  ctx->close_count++;
}

static void ws_echo_handler(xHttpResponseWriter writer, const xHttpRequest *req,
                            void *arg) {
  WsServerCtx *ctx = (WsServerCtx *)arg;
  xWsCallbacks cbs = {};
  cbs.on_open      = server_on_open;
  cbs.on_message   = server_on_message;
  cbs.on_close     = server_on_close;
  xWsUpgrade(writer, req, &cbs, ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Test fixture
 * ═══════════════════════════════════════════════════════════════════
 */

class WsConnectTest : public HttpServerTest {
protected:
  WsServerCtx server_ctx;
  WsClientCtx client_ctx;

  void SetUpEchoServer(const std::string &path = "/ws") {
    std::string pattern = "GET " + path;
    xErrno      err =
      xHttpServerRoute(server, pattern.c_str(), ws_echo_handler, &server_ctx);
    ASSERT_EQ(err, xErrno_Ok);
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST_F(WsConnectTest, ConnectAndEcho) {
  SetUpEchoServer();
  listen_and_pump();

  /* Build ws:// URL */
  char url[128];
  snprintf(url, sizeof(url), "ws://127.0.0.1:%u/ws", port);

  xWsConnectConf conf = {};
  conf.url            = url;

  xWsCallbacks cbs = {};
  cbs.on_open      = client_on_open;
  cbs.on_message   = client_on_message;
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Pump until connected */
  for (int i = 0; i < 100 && client_ctx.open_count == 0; i++) {
    pump_loop(loop, 10);
  }
  ASSERT_EQ(client_ctx.open_count.load(), 1);
  ASSERT_NE(client_ctx.conn, nullptr);

  /* Send a message */
  const char *msg = "Hello from client!";
  err             = xWsSend(client_ctx.conn, xWsOpcode_Text, msg, strlen(msg));
  ASSERT_EQ(err, xErrno_Ok);

  /* Pump until echo received */
  for (int i = 0; i < 100 && client_ctx.message_count == 0; i++) {
    pump_loop(loop, 10);
  }
  EXPECT_EQ(client_ctx.message_count.load(), 1);
  EXPECT_EQ(client_ctx.last_message, "Hello from client!");
  EXPECT_EQ(client_ctx.last_opcode, xWsOpcode_Text);

  /* Close gracefully */
  xWsClose(client_ctx.conn, 1000);
  for (int i = 0; i < 100 && client_ctx.close_count == 0; i++) {
    pump_loop(loop, 10);
  }
  EXPECT_GE(client_ctx.close_count.load(), 1);
}

TEST_F(WsConnectTest, ConnectBinaryMessage) {
  SetUpEchoServer();
  listen_and_pump();

  char url[128];
  snprintf(url, sizeof(url), "ws://127.0.0.1:%u/ws", port);

  xWsConnectConf conf = {};
  conf.url            = url;

  xWsCallbacks cbs = {};
  cbs.on_open      = client_on_open;
  cbs.on_message   = client_on_message;
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  for (int i = 0; i < 100 && client_ctx.open_count == 0; i++)
    pump_loop(loop, 10);
  ASSERT_EQ(client_ctx.open_count.load(), 1);

  /* Send binary data */
  uint8_t data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE};
  err = xWsSend(client_ctx.conn, xWsOpcode_Binary, data, sizeof(data));
  ASSERT_EQ(err, xErrno_Ok);

  for (int i = 0; i < 100 && client_ctx.message_count == 0; i++)
    pump_loop(loop, 10);
  EXPECT_EQ(client_ctx.message_count.load(), 1);
  EXPECT_EQ(client_ctx.last_opcode, xWsOpcode_Binary);
  EXPECT_EQ(client_ctx.last_message.size(), sizeof(data));

  xWsClose(client_ctx.conn, 1000);
  for (int i = 0; i < 100 && client_ctx.close_count == 0; i++)
    pump_loop(loop, 10);
}

TEST_F(WsConnectTest, InvalidUrl) {
  xWsConnectConf conf = {};
  conf.url            = "not-a-url";

  xWsCallbacks cbs = {};
  cbs.on_open      = client_on_open;
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  EXPECT_NE(err, xErrno_Ok);
  EXPECT_EQ(client_ctx.open_count.load(), 0);
}

TEST_F(WsConnectTest, NullUrl) {
  xWsConnectConf conf = {};
  conf.url            = nullptr;

  xWsCallbacks cbs = {};
  xErrno       err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST_F(WsConnectTest, WrongScheme) {
  xWsConnectConf conf = {};
  conf.url            = "http://127.0.0.1:8080/ws";

  xWsCallbacks cbs = {};
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST_F(WsConnectTest, ConnectionRefused) {
  /* Connect to a port with nothing listening */
  uint16_t dead_port = find_free_port();

  char url[128];
  snprintf(url, sizeof(url), "ws://127.0.0.1:%u/ws", dead_port);

  xWsConnectConf conf = {};
  conf.url            = url;
  conf.timeout_ms     = 2000;

  xWsCallbacks cbs = {};
  cbs.on_open      = client_on_open;
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Pump until on_close fires (connection refused) */
  for (int i = 0; i < 200 && client_ctx.close_count == 0; i++) {
    pump_loop(loop, 10);
  }
  EXPECT_EQ(client_ctx.open_count.load(), 0);
  EXPECT_GE(client_ctx.close_count.load(), 1);
}

TEST_F(WsConnectTest, ConnectWithPath) {
  SetUpEchoServer("/chat");
  listen_and_pump();

  char url[128];
  snprintf(url, sizeof(url), "ws://127.0.0.1:%u/chat", port);

  xWsConnectConf conf = {};
  conf.url            = url;

  xWsCallbacks cbs = {};
  cbs.on_open      = client_on_open;
  cbs.on_message   = client_on_message;
  cbs.on_close     = client_on_close;

  xErrno err = xWsConnect(loop, &conf, &cbs, &client_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  for (int i = 0; i < 100 && client_ctx.open_count == 0; i++)
    pump_loop(loop, 10);
  EXPECT_EQ(client_ctx.open_count.load(), 1);

  xWsClose(client_ctx.conn, 1000);
  for (int i = 0; i < 100 && client_ctx.close_count == 0; i++)
    pump_loop(loop, 10);
}
