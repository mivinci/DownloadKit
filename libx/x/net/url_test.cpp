/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * url_test.cpp - Unit tests for xUrlParse / xUrlPort
 */

#include <gtest/gtest.h>

extern "C" {
#include "url.h"
}

/* ───────────────── helpers ───────────────── */

static std::string sv(const char *p, size_t len) {
  return p ? std::string(p, len) : std::string();
}

/* ───────────────── xUrlParse ───────────────── */

TEST(UrlParse, FullUrl) {
  xUrl url;
  ASSERT_EQ(xUrlParse(
    "https://user:pass@example.com:8443/ws/chat?token=abc#top",
    &url), xErrno_Ok);

  EXPECT_EQ(sv(url.scheme, url.scheme_len), "https");
  EXPECT_EQ(sv(url.userinfo, url.userinfo_len), "user:pass");
  EXPECT_EQ(sv(url.host, url.host_len), "example.com");
  EXPECT_EQ(sv(url.port, url.port_len), "8443");
  EXPECT_EQ(sv(url.path, url.path_len), "/ws/chat");
  EXPECT_EQ(sv(url.query, url.query_len), "token=abc");
  EXPECT_EQ(sv(url.fragment, url.fragment_len), "top");
  xUrlFree(&url);
}

TEST(UrlParse, MinimalHttp) {
  xUrl url;
  ASSERT_EQ(xUrlParse("http://localhost", &url), xErrno_Ok);

  EXPECT_EQ(sv(url.scheme, url.scheme_len), "http");
  EXPECT_EQ(sv(url.host, url.host_len), "localhost");
  EXPECT_EQ(url.port, nullptr);
  EXPECT_EQ(url.path, nullptr);
  EXPECT_EQ(url.query, nullptr);
  EXPECT_EQ(url.fragment, nullptr);
  xUrlFree(&url);
}

TEST(UrlParse, WsWithPath) {
  xUrl url;
  ASSERT_EQ(xUrlParse("ws://10.0.0.1:9090/ws", &url),
            xErrno_Ok);

  EXPECT_EQ(sv(url.scheme, url.scheme_len), "ws");
  EXPECT_EQ(sv(url.host, url.host_len), "10.0.0.1");
  EXPECT_EQ(sv(url.port, url.port_len), "9090");
  EXPECT_EQ(sv(url.path, url.path_len), "/ws");
  xUrlFree(&url);
}

TEST(UrlParse, WssNoPort) {
  xUrl url;
  ASSERT_EQ(xUrlParse("wss://echo.example.com/sock", &url),
            xErrno_Ok);

  EXPECT_EQ(sv(url.scheme, url.scheme_len), "wss");
  EXPECT_EQ(sv(url.host, url.host_len), "echo.example.com");
  EXPECT_EQ(url.port, nullptr);
  EXPECT_EQ(sv(url.path, url.path_len), "/sock");
  xUrlFree(&url);
}

TEST(UrlParse, Ipv6Literal) {
  xUrl url;
  ASSERT_EQ(xUrlParse("http://[::1]:8080/test", &url),
            xErrno_Ok);

  EXPECT_EQ(sv(url.host, url.host_len), "::1");
  EXPECT_EQ(sv(url.port, url.port_len), "8080");
  EXPECT_EQ(sv(url.path, url.path_len), "/test");
  xUrlFree(&url);
}

TEST(UrlParse, Ipv6NoPort) {
  xUrl url;
  ASSERT_EQ(
    xUrlParse("http://[2001:db8::1]/index", &url),
    xErrno_Ok);

  EXPECT_EQ(sv(url.host, url.host_len), "2001:db8::1");
  EXPECT_EQ(url.port, nullptr);
  xUrlFree(&url);
}

TEST(UrlParse, QueryOnly) {
  xUrl url;
  ASSERT_EQ(xUrlParse("http://h?q=1", &url), xErrno_Ok);

  EXPECT_EQ(sv(url.host, url.host_len), "h");
  EXPECT_EQ(url.path, nullptr);
  EXPECT_EQ(sv(url.query, url.query_len), "q=1");
  xUrlFree(&url);
}

TEST(UrlParse, FragmentOnly) {
  xUrl url;
  ASSERT_EQ(xUrlParse("http://h#frag", &url), xErrno_Ok);

  EXPECT_EQ(sv(url.host, url.host_len), "h");
  EXPECT_EQ(url.path, nullptr);
  EXPECT_EQ(url.query, nullptr);
  EXPECT_EQ(sv(url.fragment, url.fragment_len), "frag");
  xUrlFree(&url);
}

/* ───────────────── error cases ───────────────── */

TEST(UrlParse, NullInput) {
  xUrl url;
  EXPECT_EQ(xUrlParse(NULL, &url), xErrno_InvalidArg);
  EXPECT_EQ(xUrlParse("http://x", NULL), xErrno_InvalidArg);
}

TEST(UrlParse, NoScheme) {
  xUrl url;
  EXPECT_EQ(xUrlParse("example.com/path", &url),
            xErrno_InvalidArg);
}

TEST(UrlParse, EmptyHost) {
  xUrl url;
  EXPECT_EQ(xUrlParse("http:///path", &url),
            xErrno_InvalidArg);
}

/* ───────────────── xUrlPort ───────────────── */

TEST(UrlPort, ExplicitPort) {
  xUrl url;
  xUrlParse("http://h:9090/p", &url);
  EXPECT_EQ(xUrlPort(&url), 9090);
  xUrlFree(&url);
}

TEST(UrlPort, DefaultHttp) {
  xUrl url;
  xUrlParse("http://h/p", &url);
  EXPECT_EQ(xUrlPort(&url), 80);
  xUrlFree(&url);
}

TEST(UrlPort, DefaultHttps) {
  xUrl url;
  xUrlParse("https://h", &url);
  EXPECT_EQ(xUrlPort(&url), 443);
  xUrlFree(&url);
}

TEST(UrlPort, DefaultWs) {
  xUrl url;
  xUrlParse("ws://h/ws", &url);
  EXPECT_EQ(xUrlPort(&url), 80);
  xUrlFree(&url);
}

TEST(UrlPort, DefaultWss) {
  xUrl url;
  xUrlParse("wss://h/ws", &url);
  EXPECT_EQ(xUrlPort(&url), 443);
  xUrlFree(&url);
}

TEST(UrlPort, UnknownScheme) {
  xUrl url;
  xUrlParse("ftp://h", &url);
  EXPECT_EQ(xUrlPort(&url), 0);
  xUrlFree(&url);
}

TEST(UrlPort, Null) {
  EXPECT_EQ(xUrlPort(NULL), 0);
}

/* ───────────────── xUrlFree ───────────────── */

TEST(UrlFree, OwnsRawCopy) {
  /* Verify xUrl is self-contained after parse */
  char *heap = strdup("ws://example.com:9090/ws?k=v#f");
  ASSERT_NE(heap, nullptr);

  xUrl url;
  ASSERT_EQ(xUrlParse(heap, &url), xErrno_Ok);

  /* Destroy the original – xUrl must still be valid */
  free(heap);

  EXPECT_EQ(sv(url.scheme, url.scheme_len), "ws");
  EXPECT_EQ(sv(url.host, url.host_len), "example.com");
  EXPECT_EQ(sv(url.port, url.port_len), "9090");
  EXPECT_EQ(sv(url.path, url.path_len), "/ws");
  EXPECT_EQ(sv(url.query, url.query_len), "k=v");
  EXPECT_EQ(sv(url.fragment, url.fragment_len), "f");
  xUrlFree(&url);

  /* After free, everything is zeroed */
  EXPECT_EQ(url.raw_, nullptr);
  EXPECT_EQ(url.scheme, nullptr);
  EXPECT_EQ(url.host, nullptr);
}

TEST(UrlFree, NullSafe) {
  xUrlFree(NULL); /* must not crash */

  xUrl url;
  memset(&url, 0, sizeof(url));
  xUrlFree(&url); /* zeroed struct is also safe */
}
