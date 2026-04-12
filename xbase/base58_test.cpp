/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base58_test.cpp - Tests for Base58 encoding/decoding
 */

#include <gtest/gtest.h>

extern "C" {
#include <xbase/base58.h>
}

#include <cstring>
#include <string>
#include <vector>

struct Base58TestCase {
  std::vector<uint8_t> raw;
  std::string encoded;
};

class Base58Test : public ::testing::TestWithParam<Base58TestCase> {};

// Test vectors from Bitcoin / well-known Base58 test cases
static const Base58TestCase kTestCases[] = {
    // Empty
    {{}, ""},
    // Single zero byte -> "1"
    {{0x00}, "1"},
    // Multiple leading zeros
    {{0x00, 0x00, 0x00}, "111"},
    // Simple values
    {{0x01}, "2"},
    {{0x39}, "z"},
    // "Hello World"
    {{0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64},
     "JxF12TrwUP45BMd"},
    // Leading zero + data
    {{0x00, 0x00, 0x28, 0x7f, 0xb4, 0xcd},
     "11233QC4"},
    // A URL-like string: "ws://abc@127.0.0.1:8080/ws"
    {{0x77, 0x73, 0x3a, 0x2f, 0x2f, 0x61, 0x62, 0x63, 0x40, 0x31, 0x32,
      0x37, 0x2e, 0x30, 0x2e, 0x30, 0x2e, 0x31, 0x3a, 0x38, 0x30, 0x38,
      0x30, 0x2f, 0x77, 0x73},
     "4f9SaPk6gLfnfemt8vbDoXD93TiuGETfZUdY"},
};

INSTANTIATE_TEST_SUITE_P(Base58, Base58Test,
                         ::testing::ValuesIn(kTestCases));

TEST_P(Base58Test, Encode) {
  const auto &tc = GetParam();
  char buf[256];
  size_t buf_len = sizeof(buf);

  int rc = xBase58Encode(tc.raw.data(), tc.raw.size(), buf, &buf_len);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(buf_len, tc.encoded.size());
  EXPECT_EQ(std::string(buf, buf_len), tc.encoded);
}

TEST_P(Base58Test, Decode) {
  const auto &tc = GetParam();
  uint8_t buf[256];
  size_t buf_len = sizeof(buf);

  int rc = xBase58Decode(tc.encoded.c_str(), tc.encoded.size(), buf, &buf_len);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(buf_len, tc.raw.size());
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + buf_len), tc.raw);
}

TEST(Base58, RoundTrip) {
  // Encode then decode should give back the original
  const uint8_t data[] = "The quick brown fox jumps over the lazy dog";
  size_t data_len = sizeof(data) - 1; // exclude null terminator

  char encoded[256];
  size_t encoded_len = sizeof(encoded);
  ASSERT_EQ(xBase58Encode(data, data_len, encoded, &encoded_len), 0);

  uint8_t decoded[256];
  size_t decoded_len = sizeof(decoded);
  ASSERT_EQ(xBase58Decode(encoded, encoded_len, decoded, &decoded_len), 0);

  ASSERT_EQ(decoded_len, data_len);
  EXPECT_EQ(memcmp(data, decoded, data_len), 0);
}

TEST(Base58, InvalidCharacter) {
  uint8_t buf[64];
  size_t buf_len = sizeof(buf);

  // '0' is not in the Base58 alphabet
  EXPECT_EQ(xBase58Decode("0invalid", 8, buf, &buf_len), -1);

  // 'O' is not in the Base58 alphabet
  buf_len = sizeof(buf);
  EXPECT_EQ(xBase58Decode("O", 1, buf, &buf_len), -1);

  // 'I' is not in the Base58 alphabet
  buf_len = sizeof(buf);
  EXPECT_EQ(xBase58Decode("I", 1, buf, &buf_len), -1);

  // 'l' is not in the Base58 alphabet
  buf_len = sizeof(buf);
  EXPECT_EQ(xBase58Decode("l", 1, buf, &buf_len), -1);
}

TEST(Base58, BufferTooSmall) {
  const uint8_t data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
  char encoded[2]; // too small
  size_t encoded_len = sizeof(encoded);
  EXPECT_EQ(xBase58Encode(data, sizeof(data), encoded, &encoded_len), -1);

  uint8_t decoded[1]; // too small
  size_t decoded_len = sizeof(decoded);
  EXPECT_EQ(xBase58Decode("JxF12TrwUP45BMd", 15, decoded, &decoded_len), -1);
}
