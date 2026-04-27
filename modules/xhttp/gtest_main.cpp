/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_main.cpp - Custom gtest main with curl global init/cleanup
 *
 * Ensures libcurl and OpenSSL global state is properly initialized
 * and cleaned up, which avoids LeakSanitizer false positives from
 * OpenSSL's internal thread-local state on TLS handshake failures.
 */

#include <gtest/gtest.h>
#include <curl/curl.h>

int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  ::testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();
  curl_global_cleanup();
  return result;
}
