/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_test.cpp - WebSocket unit tests
 */

#include "server_test_helper.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "ws_crypto.h"
#include "ws_frame.h"
#include <xhttp/server.h>
#include <xhttp/ws.h>
#include <xbuf/io.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Crypto backend tests
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST(WsCrypto, SHA1_KnownVector) {
  /* SHA-1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709 */
  unsigned char digest[XWS_SHA1_DIGEST_SIZE];
  xWsSHA1((const unsigned char *)"", 0, digest);

  unsigned char expected[] = {
    0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
    0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09,
  };
  EXPECT_EQ(memcmp(digest, expected, XWS_SHA1_DIGEST_SIZE), 0);
}

TEST(WsCrypto, SHA1_HelloWorld) {
  /* SHA-1("Hello, World!") known value */
  unsigned char digest[XWS_SHA1_DIGEST_SIZE];
  const char *input = "Hello, World!";
  xWsSHA1((const unsigned char *)input, strlen(input), digest);

  /* Verify it's not all zeros (basic sanity) */
  bool all_zero = true;
  for (int i = 0; i < XWS_SHA1_DIGEST_SIZE; i++) {
    if (digest[i] != 0) { all_zero = false; break; }
  }
  EXPECT_FALSE(all_zero);
}

TEST(WsCrypto, SHA1_WebSocketAccept) {
  /* RFC 6455 §4.2.2 example:
   * Key = "dGhlIHNhbXBsZSBub25jZQ=="
   * Accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
  const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
  const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  std::string concat = std::string(key) + guid;
  unsigned char digest[XWS_SHA1_DIGEST_SIZE];
  xWsSHA1((const unsigned char *)concat.c_str(),
          concat.size(), digest);

  char b64[64];
  int n = xWsBase64Encode(digest, XWS_SHA1_DIGEST_SIZE,
                          b64, sizeof(b64));
  ASSERT_GT(n, 0);
  EXPECT_STREQ(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsCrypto, Base64_Empty) {
  char out[8];
  int n = xWsBase64Encode((const unsigned char *)"", 0,
                          out, sizeof(out));
  ASSERT_GE(n, 0);
  EXPECT_STREQ(out, "");
}

TEST(WsCrypto, Base64_Padding) {
  /* "a" -> "YQ==" */
  char out[8];
  int n = xWsBase64Encode((const unsigned char *)"a", 1,
                          out, sizeof(out));
  ASSERT_GT(n, 0);
  EXPECT_STREQ(out, "YQ==");

  /* "ab" -> "YWI=" */
  n = xWsBase64Encode((const unsigned char *)"ab", 2,
                      out, sizeof(out));
  ASSERT_GT(n, 0);
  EXPECT_STREQ(out, "YWI=");

  /* "abc" -> "YWJj" */
  n = xWsBase64Encode((const unsigned char *)"abc", 3,
                      out, sizeof(out));
  ASSERT_GT(n, 0);
  EXPECT_STREQ(out, "YWJj");
}

TEST(WsCrypto, Base64_BufferTooSmall) {
  char out[2]; /* Too small */
  int n = xWsBase64Encode((const unsigned char *)"abc", 3,
                          out, sizeof(out));
  EXPECT_EQ(n, -1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Frame codec tests
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Helper: build a masked client frame in a buffer */
static std::vector<uint8_t> build_client_frame(
    uint8_t fin, uint8_t opcode,
    const void *payload, size_t len,
    const uint8_t mask_key[4]) {
  std::vector<uint8_t> frame;

  frame.push_back((uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

  /* Payload length with MASK bit set */
  if (len < 126) {
    frame.push_back((uint8_t)(0x80 | len));
  } else if (len <= 0xFFFF) {
    frame.push_back(0x80 | 126);
    frame.push_back((uint8_t)(len >> 8));
    frame.push_back((uint8_t)(len));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) {
      frame.push_back((uint8_t)(len >> (i * 8)));
    }
  }

  /* Masking key */
  frame.insert(frame.end(), mask_key, mask_key + 4);

  /* Masked payload */
  const uint8_t *p = (const uint8_t *)payload;
  for (size_t i = 0; i < len; i++) {
    frame.push_back(p[i] ^ mask_key[i & 3]);
  }

  return frame;
}

TEST(WsFrame, ParseTextFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_TEXT, "Hello", 5, mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.fin, 1);
  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_TEXT);
  EXPECT_EQ(parser.frame.payload_len, 5u);
  EXPECT_EQ(memcmp(parser.frame.payload, "Hello", 5), 0);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

TEST(WsFrame, ParseBinaryFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  uint8_t mask_key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_BINARY, data, sizeof(data), mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_BINARY);
  EXPECT_EQ(parser.frame.payload_len, 5u);
  EXPECT_EQ(memcmp(parser.frame.payload, data, 5), 0);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

TEST(WsFrame, ParseEmptyFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  uint8_t mask_key[4] = {0x00, 0x00, 0x00, 0x00};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_TEXT, nullptr, 0, mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.payload_len, 0u);
  EXPECT_EQ(parser.frame.payload, nullptr);

  xIOBufferDeinit(&io);
}

TEST(WsFrame, ParseMediumPayload) {
  /* 16-bit extended length (126-65535) */
  xIOBuffer io;
  xIOBufferInit(&io);

  std::string payload(200, 'X');
  uint8_t mask_key[4] = {0x11, 0x22, 0x33, 0x44};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_TEXT, payload.data(), payload.size(), mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.payload_len, 200u);
  EXPECT_EQ(memcmp(parser.frame.payload, payload.data(), 200), 0);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

TEST(WsFrame, ParseCloseFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Close frame with status code 1000 */
  uint8_t close_payload[] = {0x03, 0xE8}; /* 1000 in big-endian */
  uint8_t mask_key[4] = {0x00, 0x00, 0x00, 0x00};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_CLOSE, close_payload, 2, mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_CLOSE);
  EXPECT_EQ(parser.frame.payload_len, 2u);

  uint16_t code = (uint16_t)((parser.frame.payload[0] << 8) |
                              parser.frame.payload[1]);
  EXPECT_EQ(code, 1000);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

TEST(WsFrame, ParsePingFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  uint8_t mask_key[4] = {0x55, 0x66, 0x77, 0x88};
  auto frame_data = build_client_frame(
    1, XWS_OPCODE_PING, "ping", 4, mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);
  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_PING);
  EXPECT_EQ(memcmp(parser.frame.payload, "ping", 4), 0);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

TEST(WsFrame, RejectUnmaskedClientFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Build an unmasked frame (server-style) */
  uint8_t hdr[] = {0x81, 0x05}; /* FIN + TEXT, len=5, no MASK */
  xIOBufferAppend(&io, hdr, 2);
  xIOBufferAppend(&io, "Hello", 5);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  EXPECT_EQ(result, xWsFrameResult_Error);

  xIOBufferDeinit(&io);
}

TEST(WsFrame, RejectFragmentedControlFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Ping frame with FIN=0 (fragmented control frame = protocol error) */
  uint8_t mask_key[4] = {0x00, 0x00, 0x00, 0x00};
  auto frame_data = build_client_frame(
    0, XWS_OPCODE_PING, "ping", 4, mask_key);

  xIOBufferAppend(&io, frame_data.data(), frame_data.size());

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  EXPECT_EQ(result, xWsFrameResult_Error);

  xIOBufferDeinit(&io);
}

TEST(WsFrame, NeedMore) {
  xIOBuffer io;
  xIOBufferInit(&io);

  /* Only 1 byte of a 2-byte header */
  uint8_t partial[] = {0x81};
  xIOBufferAppend(&io, partial, 1);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, 1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  EXPECT_EQ(result, xWsFrameResult_NeedMore);

  xIOBufferDeinit(&io);
}

TEST(WsFrame, EncodeTextFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  int ret = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT, "Hello", 5,
                           0);
  ASSERT_EQ(ret, 0);

  /* Server frame: FIN=1, TEXT, no mask, len=5 */
  size_t total = xIOBufferLen(&io);
  EXPECT_EQ(total, 2u + 5u); /* 2-byte header + 5-byte payload */

  uint8_t buf[16];
  xIOBufferRead(&io, buf, total);

  EXPECT_EQ(buf[0], 0x81); /* FIN + TEXT */
  EXPECT_EQ(buf[1], 0x05); /* len=5, no MASK */
  EXPECT_EQ(memcmp(buf + 2, "Hello", 5), 0);

  xIOBufferDeinit(&io);
}

TEST(WsFrame, EncodeCloseFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  int ret = xWsFrameEncodeClose(&io, 1000, "bye", 3, 0);
  ASSERT_EQ(ret, 0);

  size_t total = xIOBufferLen(&io);
  EXPECT_EQ(total, 2u + 2u + 3u); /* header + code + reason */

  uint8_t buf[16];
  xIOBufferRead(&io, buf, total);

  EXPECT_EQ(buf[0], 0x88); /* FIN + CLOSE */
  EXPECT_EQ(buf[1], 0x05); /* len=5 */
  EXPECT_EQ(buf[2], 0x03); /* 1000 >> 8 */
  EXPECT_EQ(buf[3], 0xE8); /* 1000 & 0xFF */
  EXPECT_EQ(memcmp(buf + 4, "bye", 3), 0);

  xIOBufferDeinit(&io);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Integration tests (handshake + messaging)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Shared state for WS callbacks */
struct WsTestCtx {
  std::atomic<int> open_count{0};
  std::atomic<int> message_count{0};
  std::atomic<int> close_count{0};
  std::string      last_message;
  xWsOpcode        last_opcode{xWsOpcode_Text};
  uint16_t         close_code{0};
  xWsConn          last_conn{nullptr};
};

static void ws_test_on_open(xWsConn conn, void *arg) {
  auto *ctx = (WsTestCtx *)arg;
  ctx->open_count++;
  ctx->last_conn = conn;
}

static void ws_test_on_message(xWsConn conn, xWsOpcode opcode,
                               const void *payload, size_t len,
                               void *arg) {
  (void)conn;
  auto *ctx = (WsTestCtx *)arg;
  ctx->message_count++;
  ctx->last_opcode = opcode;
  if (payload && len > 0) {
    ctx->last_message = std::string((const char *)payload, len);
  } else {
    ctx->last_message.clear();
  }
}

static void ws_test_on_close(xWsConn conn, uint16_t code,
                             const char *reason, size_t len,
                             void *arg) {
  (void)conn;
  (void)reason;
  (void)len;
  auto *ctx = (WsTestCtx *)arg;
  ctx->close_count++;
  ctx->close_code = code;
}

/* Helper: perform WebSocket handshake on a raw socket */
static std::string ws_handshake_request(const std::string &path) {
  return "GET " + path + " HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
         "Sec-WebSocket-Version: 13\r\n"
         "\r\n";
}

/* Helper: send a masked client frame over a raw socket */
static bool ws_send_frame(int fd, uint8_t fin, uint8_t opcode,
                          const void *payload, size_t len) {
  uint8_t mask_key[4] = {0x37, 0xfa, 0x21, 0x3d};
  auto frame = build_client_frame(fin, opcode, payload, len,
                                  mask_key);
  ssize_t n = send(fd, frame.data(), frame.size(), 0);
  return n == (ssize_t)frame.size();
}

/* Helper: receive and parse a server frame from a raw socket */
struct RecvFrame {
  uint8_t     opcode;
  bool        fin;
  std::string payload;
  bool        valid;
};

static RecvFrame ws_recv_frame(int fd, int timeout_ms = 2000) {
  RecvFrame result = {0, false, "", false};

  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Read header (2 bytes minimum) */
  uint8_t hdr[2];
  ssize_t n = recv(fd, hdr, 2, MSG_WAITALL);
  if (n != 2) return result;

  result.fin    = (hdr[0] >> 7) & 1;
  result.opcode = hdr[0] & 0x0F;
  uint8_t len7  = hdr[1] & 0x7F;
  bool masked   = (hdr[1] >> 7) & 1;

  uint64_t payload_len = len7;
  if (len7 == 126) {
    uint8_t ext[2];
    if (recv(fd, ext, 2, MSG_WAITALL) != 2) return result;
    payload_len = ((uint64_t)ext[0] << 8) | ext[1];
  } else if (len7 == 127) {
    uint8_t ext[8];
    if (recv(fd, ext, 8, MSG_WAITALL) != 8) return result;
    payload_len = 0;
    for (int i = 0; i < 8; i++)
      payload_len = (payload_len << 8) | ext[i];
  }

  /* Skip mask key if present (server frames shouldn't be masked) */
  if (masked) {
    uint8_t mask[4];
    if (recv(fd, mask, 4, MSG_WAITALL) != 4) return result;
  }

  /* Read payload */
  if (payload_len > 0) {
    result.payload.resize((size_t)payload_len);
    n = recv(fd, &result.payload[0], (size_t)payload_len,
             MSG_WAITALL);
    if (n != (ssize_t)payload_len) return result;
  }

  result.valid = true;
  return result;
}

/* Handler that upgrades to WebSocket */
static void ws_upgrade_handler(xHttpResponseWriter writer,
                               const xHttpRequest *req,
                               void *arg) {
  WsTestCtx *ctx = (WsTestCtx *)arg;
  xWsCallbacks cbs = {};
  cbs.on_open    = ws_test_on_open;
  cbs.on_message = ws_test_on_message;
  cbs.on_close   = ws_test_on_close;

  xWsUpgrade(writer, req, &cbs, ctx);
}

class WsServerTest : public HttpServerTest {
protected:
  WsTestCtx ws_ctx;

  void SetUpWsRoute(const std::string &path = "/ws") {
    std::string pattern = "GET " + path;
    xErrno err = xHttpServerRoute(server, pattern.c_str(),
                                  ws_upgrade_handler, &ws_ctx);
    ASSERT_EQ(err, xErrno_Ok);
  }

  /* Connect and perform WS handshake, return the fd */
  int ws_connect(const std::string &path = "/ws") {
    int fd = connect_to(port);
    if (fd < 0) return -1;

    std::string req = ws_handshake_request(path);
    if (!send_str(fd, req)) {
      close(fd);
      return -1;
    }

    /* Pump loop to process the handshake */
    pump_loop(loop, 50);

    /* Read the 101 response */
    std::string resp = recv_all(fd, 1000);
    if (resp.find("101") == std::string::npos) {
      close(fd);
      return -1;
    }

    return fd;
  }
};

TEST_F(WsServerTest, HandshakeSuccess) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0) << "WebSocket handshake failed";

  pump_loop(loop, 50);
  EXPECT_EQ(ws_ctx.open_count.load(), 1);

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, HandshakeMissingHeaders) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* Send a GET without WebSocket headers */
  std::string req =
    "GET /ws HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "\r\n";
  ASSERT_TRUE(send_str(fd, req));

  pump_loop(loop, 50);

  std::string resp = recv_all(fd, 1000);
  EXPECT_NE(resp.find("400"), std::string::npos)
    << "Expected 400 Bad Request, got: " << resp;

  close(fd);
  pump_loop(loop, 50);
}

TEST_F(WsServerTest, HandshakeWrongVersion) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string req =
    "GET /ws HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 8\r\n"
    "\r\n";
  ASSERT_TRUE(send_str(fd, req));

  pump_loop(loop, 50);

  std::string resp = recv_all(fd, 1000);
  EXPECT_NE(resp.find("400"), std::string::npos)
    << "Expected 400 for wrong version, got: " << resp;

  close(fd);
  pump_loop(loop, 50);
}

TEST_F(WsServerTest, HandshakeWrongMethod) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* POST to a GET-only route should get 405 from the router */
  std::string req =
    "POST /ws HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";
  ASSERT_TRUE(send_str(fd, req));

  pump_loop(loop, 50);

  std::string resp = recv_all(fd, 1000);
  /* Router returns 405 because path matches but method doesn't */
  EXPECT_TRUE(resp.find("405") != std::string::npos ||
              resp.find("404") != std::string::npos)
    << "Expected 405 or 404 for POST, got: " << resp;

  close(fd);
  pump_loop(loop, 50);
}

TEST_F(WsServerTest, TextMessage) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Send a text message */
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_TEXT,
                            "Hello WS", 8));
  pump_loop(loop, 100);

  EXPECT_EQ(ws_ctx.message_count.load(), 1);
  EXPECT_EQ(ws_ctx.last_message, "Hello WS");
  EXPECT_EQ(ws_ctx.last_opcode, xWsOpcode_Text);

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, BinaryMessage) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_BINARY,
                            data, sizeof(data)));
  pump_loop(loop, 100);

  EXPECT_EQ(ws_ctx.message_count.load(), 1);
  EXPECT_EQ(ws_ctx.last_opcode, xWsOpcode_Binary);
  EXPECT_EQ(ws_ctx.last_message.size(), 4u);

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, PingPong) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Send a Ping */
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_PING,
                            "ping", 4));
  pump_loop(loop, 100);

  /* Should receive a Pong with same payload */
  auto pong = ws_recv_frame(fd);
  ASSERT_TRUE(pong.valid) << "Failed to receive Pong frame";
  EXPECT_EQ(pong.opcode, XWS_OPCODE_PONG);
  EXPECT_EQ(pong.payload, "ping");

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, CloseHandshake) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Send Close frame with code 1000 */
  uint8_t close_payload[] = {0x03, 0xE8}; /* 1000 */
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_CLOSE,
                            close_payload, 2));
  pump_loop(loop, 100);

  /* Should receive a Close frame back */
  auto close_frame = ws_recv_frame(fd);
  ASSERT_TRUE(close_frame.valid) << "Failed to receive Close frame";
  EXPECT_EQ(close_frame.opcode, XWS_OPCODE_CLOSE);

  pump_loop(loop, 100);
  EXPECT_EQ(ws_ctx.close_count.load(), 1);
  EXPECT_EQ(ws_ctx.close_code, 1000);

  close(fd);
  pump_loop(loop, 50);
}

TEST_F(WsServerTest, ServerSend) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Send a message from server to client */
  ASSERT_NE(ws_ctx.last_conn, nullptr);
  xErrno err = xWsSend(ws_ctx.last_conn, xWsOpcode_Text,
                       "from server", 11);
  EXPECT_EQ(err, xErrno_Ok);
  pump_loop(loop, 100);

  /* Receive the frame on the client side */
  auto frame = ws_recv_frame(fd);
  ASSERT_TRUE(frame.valid) << "Failed to receive server message";
  EXPECT_EQ(frame.opcode, XWS_OPCODE_TEXT);
  EXPECT_EQ(frame.payload, "from server");

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, FragmentedMessage) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Send fragmented message: "Hello" + " " + "World" */
  /* Fragment 1: TEXT, FIN=0 */
  ASSERT_TRUE(ws_send_frame(fd, 0, XWS_OPCODE_TEXT,
                            "Hello", 5));
  pump_loop(loop, 50);
  EXPECT_EQ(ws_ctx.message_count.load(), 0)
    << "Should not deliver until final fragment";

  /* Fragment 2: CONTINUATION, FIN=0 */
  ASSERT_TRUE(ws_send_frame(fd, 0, XWS_OPCODE_CONTINUATION,
                            " ", 1));
  pump_loop(loop, 50);
  EXPECT_EQ(ws_ctx.message_count.load(), 0);

  /* Fragment 3: CONTINUATION, FIN=1 */
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_CONTINUATION,
                            "World", 5));
  pump_loop(loop, 100);

  EXPECT_EQ(ws_ctx.message_count.load(), 1);
  EXPECT_EQ(ws_ctx.last_message, "Hello World");
  EXPECT_EQ(ws_ctx.last_opcode, xWsOpcode_Text);

  close(fd);
  pump_loop(loop, 100);
}

TEST_F(WsServerTest, ServerInitiatedClose) {
  SetUpWsRoute();
  listen_and_pump();

  int fd = ws_connect();
  ASSERT_GE(fd, 0);
  pump_loop(loop, 50);

  /* Server initiates close */
  ASSERT_NE(ws_ctx.last_conn, nullptr);
  xErrno err = xWsClose(ws_ctx.last_conn, 1000);
  EXPECT_EQ(err, xErrno_Ok);
  pump_loop(loop, 100);

  /* Client should receive Close frame */
  auto close_frame = ws_recv_frame(fd);
  ASSERT_TRUE(close_frame.valid) << "Failed to receive Close frame";
  EXPECT_EQ(close_frame.opcode, XWS_OPCODE_CLOSE);

  /* Client responds with Close */
  uint8_t close_payload[] = {0x03, 0xE8};
  ASSERT_TRUE(ws_send_frame(fd, 1, XWS_OPCODE_CLOSE,
                            close_payload, 2));
  pump_loop(loop, 100);

  EXPECT_EQ(ws_ctx.close_count.load(), 1);

  close(fd);
  pump_loop(loop, 50);
}
