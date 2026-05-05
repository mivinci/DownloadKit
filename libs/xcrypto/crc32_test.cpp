/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * crc32_test.cpp - Unit tests for xCrc32
 */

#include <xcrypto/crc32.h>

#include <gtest/gtest.h>

#include <cstring>

TEST(Crc32, Empty) {
  EXPECT_EQ(xCrc32((const uint8_t *)"", 0), 0x00000000u);
}

TEST(Crc32, CheckValue) {
  /* The CRC-32 "check value" for the string "123456789" is 0xCBF43926 */
  const char *input = "123456789";
  EXPECT_EQ(xCrc32((const uint8_t *)input, strlen(input)), 0xCBF43926u);
}

TEST(Crc32, Deterministic) {
  const char *input = "Hello, World!";
  uint32_t    crc1  = xCrc32((const uint8_t *)input, strlen(input));
  uint32_t    crc2  = xCrc32((const uint8_t *)input, strlen(input));
  EXPECT_EQ(crc1, crc2);
}

TEST(Crc32, DifferentInputs) {
  const char *a = "foo";
  const char *b = "bar";
  EXPECT_NE(xCrc32((const uint8_t *)a, strlen(a)),
            xCrc32((const uint8_t *)b, strlen(b)));
}
