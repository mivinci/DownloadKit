/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error_test.cpp - Unit tests for xstrerror
 */

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include <x/base/error.h>
}

/* ── Valid error codes return correct strings ── */

TEST(ErrorTest, OkReturnsOk) {
  EXPECT_STREQ(xstrerror(xErrno_Ok), "ok");
}

TEST(ErrorTest, UnknownReturnsUnknown) {
  EXPECT_STREQ(xstrerror(xErrno_Unknown), "unknown error");
}

TEST(ErrorTest, InvalidArgReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_InvalidArg), "invalid argument");
}

TEST(ErrorTest, NoMemoryReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_NoMemory), "out of memory");
}

TEST(ErrorTest, InvalidStateReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_InvalidState), "invalid state");
}

TEST(ErrorTest, SysErrorReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_SysError), "system error");
}

TEST(ErrorTest, NotFoundReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_NotFound), "not found");
}

TEST(ErrorTest, AlreadyExistsReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_AlreadyExists), "already exists");
}

TEST(ErrorTest, CancelledReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_Cancelled), "cancelled");
}

TEST(ErrorTest, NotSupportedReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_NotSupported), "not supported");
}

TEST(ErrorTest, DnsNotFoundReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_DnsNotFound), "dns: hostname not found");
}

TEST(ErrorTest, DnsTempFailReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_DnsTempFail), "dns: temporary failure");
}

TEST(ErrorTest, DnsErrorReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_DnsError), "dns: resolution error");
}

TEST(ErrorTest, TimeoutReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_Timeout), "operation timed out");
}

TEST(ErrorTest, AgainReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_Again), "try again");
}

TEST(ErrorTest, BusyReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_Busy), "resource busy");
}

TEST(ErrorTest, PromptTooLongReturnsCorrectString) {
  EXPECT_STREQ(xstrerror(xErrno_PromptTooLong),
               "prompt exceeds context budget");
}

/* ── Out-of-range error codes return "unknown error" ── */

TEST(ErrorTest, NegativeCodeReturnsUnknown) {
  EXPECT_STREQ(xstrerror((xErrno)-1), "unknown error");
}

TEST(ErrorTest, LargeCodeReturnsUnknown) {
  EXPECT_STREQ(xstrerror((xErrno)9999), "unknown error");
}

/* ── Return value is never NULL ── */

TEST(ErrorTest, ReturnValueNeverNull) {
  for (int i = -10; i < 100; i++) {
    const char *msg = xstrerror((xErrno)i);
    ASSERT_NE(msg, nullptr) << "xstrerror(" << i << ") returned NULL";
    EXPECT_GT(std::strlen(msg), 0u) << "xstrerror(" << i << ") returned empty string";
  }
}
