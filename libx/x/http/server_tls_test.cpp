/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_tls_test.cpp - TLS integration tests for xhttp
 */

#include "server_test_helper.h"

#include <atomic>
#include <thread>

extern "C" {
#include <x/base/error.h>
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Tests that work regardless of TLS backend availability
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_F(HttpServerTest, ListenTLS_NullServerReturnsError) {
  xTlsConf config = {};
  config.cert     = "/tmp/cert.pem";
  config.key      = "/tmp/key.pem";
  EXPECT_EQ(xHttpServerListenTls(nullptr, "127.0.0.1", port, &config), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, ListenTLS_NullConfigReturnsError) {
  EXPECT_EQ(xHttpServerListenTls(server, "127.0.0.1", port, nullptr), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, ListenTLS_NullCertFileReturnsError) {
  xTlsConf config = {};
  config.cert     = nullptr;
  config.key      = "/tmp/key.pem";
  EXPECT_EQ(xHttpServerListenTls(server, "127.0.0.1", port, &config), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, ListenTLS_NullKeyFileReturnsError) {
  xTlsConf config = {};
  config.cert     = "/tmp/cert.pem";
  config.key      = nullptr;
  EXPECT_EQ(xHttpServerListenTls(server, "127.0.0.1", port, &config), xErrno_InvalidArg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TLS integration tests (only when a TLS backend is available)
 * ═══════════════════════════════════════════════════════════════════════════
 */

#if defined(X_HAS_OPENSSL)

#include <openssl/err.h>
#include <openssl/ssl.h>

/**
 * @brief Test fixture for TLS integration tests.
 *
 * Generates a self-signed certificate in SetUp() and cleans up in TearDown().
 * Runs the event loop in a background thread so that blocking SSL_connect()
 * on the client side can complete while the server processes events.
 */
class HttpServerTlsTest : public ::testing::Test {
protected:
  xEventLoop  loop     = nullptr;
  xHttpServer server   = nullptr;
  uint16_t    port     = 0;
  uint16_t    tls_port = 0;

  std::string cert_path;
  std::string key_path;

  std::atomic<bool> loop_running{false};
  std::thread       loop_thread;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    server = xHttpServerCreate(loop);
    ASSERT_NE(server, nullptr);

    port     = find_free_port();
    tls_port = find_free_port();
    ASSERT_NE(port, 0);
    ASSERT_NE(tls_port, 0);
    ASSERT_NE(port, tls_port);

    /* Generate self-signed certificate */
    cert_path = "/tmp/xhttp_test_cert.pem";
    key_path  = "/tmp/xhttp_test_key.pem";

    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path + " -out " +
                      cert_path + " -days 1 -nodes -subj '/CN=localhost' 2>/dev/null";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "Failed to generate self-signed certificate";
  }

  void TearDown() override {
    stop_loop();
    if (server) xHttpServerDestroy(server);
    if (loop) xEventLoopDestroy(loop);

    unlink(cert_path.c_str());
    unlink(key_path.c_str());
  }

  /** Start the event loop in a background thread. */
  void start_loop() {
    loop_running = true;
    loop_thread  = std::thread([this]() {
      while (loop_running.load()) {
        xEventWait(loop, 5);
      }
    });
  }

  /** Stop the background event loop thread. */
  void stop_loop() {
    loop_running = false;
    if (loop_thread.joinable()) loop_thread.join();
  }

  /** Start TLS listening and start the event loop thread. */
  void listen_tls_and_start() {
    xTlsConf config    = {};
    config.cert        = cert_path.c_str();
    config.key         = key_path.c_str();
    config.skip_verify = 1;

    xErrno err = xHttpServerListenTls(server, "127.0.0.1", tls_port, &config);
    ASSERT_EQ(err, xErrno_Ok) << "Failed to listen TLS on port " << tls_port;
    start_loop();
    /* Give the loop a moment to start processing */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  /**
   * @brief Connect via TLS and perform handshake (blocking).
   *        The event loop runs in a background thread so the server
   *        can process the handshake concurrently.
   */
  struct TlsConn {
    SSL     *ssl;
    SSL_CTX *ctx;
    int      fd;
  };

  TlsConn connect_tls(const char *alpn_proto = nullptr) {
    TlsConn conn = {nullptr, nullptr, -1};

    conn.ctx = SSL_CTX_new(TLS_client_method());
    if (!conn.ctx) return conn;

    /* Don't verify server certificate (self-signed) */
    SSL_CTX_set_verify(conn.ctx, SSL_VERIFY_NONE, nullptr);

    /* Set ALPN if requested */
    if (alpn_proto) {
      size_t                     proto_len = strlen(alpn_proto);
      std::vector<unsigned char> alpn_buf(proto_len + 1);
      alpn_buf[0] = (unsigned char)proto_len;
      memcpy(&alpn_buf[1], alpn_proto, proto_len);
      SSL_CTX_set_alpn_protos(conn.ctx, alpn_buf.data(), (unsigned)alpn_buf.size());
    }

    conn.fd = connect_to(tls_port);
    if (conn.fd < 0) {
      SSL_CTX_free(conn.ctx);
      conn.ctx = nullptr;
      return conn;
    }

    conn.ssl = SSL_new(conn.ctx);
    if (!conn.ssl) {
      close(conn.fd);
      SSL_CTX_free(conn.ctx);
      conn.fd  = -1;
      conn.ctx = nullptr;
      return conn;
    }

    SSL_set_fd(conn.ssl, conn.fd);

    /* Perform blocking handshake (server loop runs in background thread) */
    int ret = SSL_connect(conn.ssl);
    if (ret != 1) {
      SSL_free(conn.ssl);
      close(conn.fd);
      SSL_CTX_free(conn.ctx);
      conn.ssl = nullptr;
      conn.fd  = -1;
      conn.ctx = nullptr;
      return conn;
    }

    return conn;
  }

  void close_tls(TlsConn &conn) {
    if (conn.ssl) {
      SSL_shutdown(conn.ssl);
      SSL_free(conn.ssl);
    }
    if (conn.fd >= 0) close(conn.fd);
    if (conn.ctx) SSL_CTX_free(conn.ctx);
    conn.ssl = nullptr;
    conn.fd  = -1;
    conn.ctx = nullptr;
  }

  std::string tls_send_recv(TlsConn &conn, const std::string &request, int timeout_ms = 2000) {
    if (!request.empty()) {
      SSL_write(conn.ssl, request.data(), (int)request.size());
    }

    std::string result;
    char        buf[4096];

    /* Set socket timeout for reads */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
      int n = SSL_read(conn.ssl, buf, sizeof(buf));
      if (n <= 0) break;
      result.append(buf, (size_t)n);

      /* Check for complete HTTP response */
      if (result.find("\r\n\r\n") != std::string::npos) {
        auto cl_pos = result.find("Content-Length: ");
        if (cl_pos != std::string::npos) {
          size_t cl_start = cl_pos + 16;
          size_t cl_end   = result.find("\r\n", cl_start);
          if (cl_end != std::string::npos) {
            int    content_len = std::stoi(result.substr(cl_start, cl_end - cl_start));
            size_t body_start  = result.find("\r\n\r\n") + 4;
            if (result.size() >= body_start + (size_t)content_len) break;
          }
        } else {
          break;
        }
      }
    }
    return result;
  }
};

/* ── Basic TLS connection ─────────────────────────────────────────────── */

static void tls_hello_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)req;
  (void)arg;
  const char *body = "Hello TLS!";
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  xHttpResponseWrite(w, body, strlen(body));
  xHttpResponseEnd(w);
}

TEST_F(HttpServerTlsTest, BasicTlsConnection) {
  xHttpServerRoute(server, "GET /hello", tls_hello_handler, nullptr);
  listen_tls_and_start();

  TlsConn conn = connect_tls("http/1.1");
  ASSERT_NE(conn.ssl, nullptr) << "TLS handshake failed";

  std::string response = tls_send_recv(conn, "GET /hello HTTP/1.1\r\nHost: localhost\r\n"
                                             "Connection: close\r\n\r\n");

  EXPECT_TRUE(response.find("200") != std::string::npos) << "Expected 200 OK, got: " << response;
  EXPECT_TRUE(response.find("Hello TLS!") != std::string::npos)
    << "Expected body 'Hello TLS!', got: " << response;

  close_tls(conn);
}

/* ── ALPN negotiation: http/1.1 ───────────────────────────────────────── */

TEST_F(HttpServerTlsTest, AlpnNegotiatesH1) {
  xHttpServerRoute(server, "GET /alpn", tls_hello_handler, nullptr);
  listen_tls_and_start();

  TlsConn conn = connect_tls("http/1.1");
  ASSERT_NE(conn.ssl, nullptr) << "TLS handshake failed";

  /* Check ALPN result */
  const unsigned char *alpn_data = nullptr;
  unsigned int         alpn_len  = 0;
  SSL_get0_alpn_selected(conn.ssl, &alpn_data, &alpn_len);

  if (alpn_data && alpn_len > 0) {
    std::string alpn_result((const char *)alpn_data, alpn_len);
    EXPECT_EQ(alpn_result, "http/1.1");
  }

  close_tls(conn);
}

/* ── Invalid certificate path ─────────────────────────────────────────── */

TEST_F(HttpServerTlsTest, InvalidCertPathReturnsError) {
  xTlsConf config    = {};
  config.cert        = "/nonexistent/cert.pem";
  config.key         = key_path.c_str();
  config.skip_verify = 1;

  xErrno err = xHttpServerListenTls(server, "127.0.0.1", tls_port, &config);
  EXPECT_EQ(err, xErrno_SysError);
}

TEST_F(HttpServerTlsTest, InvalidKeyPathReturnsError) {
  xTlsConf config    = {};
  config.cert        = cert_path.c_str();
  config.key         = "/nonexistent/key.pem";
  config.skip_verify = 1;

  xErrno err = xHttpServerListenTls(server, "127.0.0.1", tls_port, &config);
  EXPECT_EQ(err, xErrno_SysError);
}

/* ── Simultaneous HTTP and HTTPS ──────────────────────────────────────── */

TEST_F(HttpServerTlsTest, SimultaneousHttpAndHttps) {
  xHttpServerRoute(server, "GET /dual", tls_hello_handler, nullptr);

  /* Start plain HTTP */
  xErrno err = xHttpServerListen(server, "127.0.0.1", port);
  ASSERT_EQ(err, xErrno_Ok);

  /* Start HTTPS and event loop */
  listen_tls_and_start();

  /* Test plain HTTP */
  int plain_fd = connect_to(port);
  ASSERT_GE(plain_fd, 0);
  send_str(plain_fd, "GET /dual HTTP/1.1\r\nHost: localhost\r\n"
                     "Connection: close\r\n\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string plain_response = recv_all(plain_fd);
  close(plain_fd);

  EXPECT_TRUE(plain_response.find("200") != std::string::npos)
    << "Plain HTTP failed: " << plain_response;

  /* Test HTTPS */
  TlsConn tls_conn = connect_tls("http/1.1");
  ASSERT_NE(tls_conn.ssl, nullptr) << "TLS handshake failed";

  std::string tls_response = tls_send_recv(tls_conn, "GET /dual HTTP/1.1\r\nHost: localhost\r\n"
                                                     "Connection: close\r\n\r\n");

  EXPECT_TRUE(tls_response.find("200") != std::string::npos) << "HTTPS failed: " << tls_response;

  close_tls(tls_conn);
}

#elif defined(X_HAS_MBEDTLS)

/* mbedTLS client-side tests would go here.
 * For now, we only test the stub behavior. */

#else /* No TLS backend */

TEST_F(HttpServerTest, ListenTLS_NoBackendReturnsNotSupported) {
  xTlsServerConf config = {};
  config.cert           = "/tmp/cert.pem";
  config.key            = "/tmp/key.pem";
  EXPECT_EQ(xHttpServerListenTls(server, "127.0.0.1", port, &config), xErrno_NotSupported);
}

#endif /* X_HAS_OPENSSL / X_HAS_MBEDTLS */
