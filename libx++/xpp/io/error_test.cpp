/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error_test.cpp - Unit tests for xpp::io::Error.
 */

#include <gtest/gtest.h>
#include <xpp/io/error.h>

#include <cerrno>
#include <cstring>
#include <type_traits>

using xpp::io::errno_to_kind;
using xpp::io::Error;
using xpp::io::ErrorKind;
using xpp::io::kind_name;

/* ── Compile-time invariants ─────────────────────────────────────── */

static_assert(sizeof(Error) == sizeof(void *), "Error must be one pointer wide");
static_assert(sizeof(xpp::Option<Error>) == sizeof(void *), "Option<Error> niche broken");
static_assert(!std::is_copy_constructible<Error>::value, "Error must be move-only");
static_assert(!std::is_copy_assignable<Error>::value, "Error must be move-only");
static_assert(std::is_move_constructible<Error>::value, "Error must be movable");
static_assert(std::is_nothrow_move_constructible<Error>::value,
              "Error move ctor should be noexcept");

/* ── ErrorKind name table ────────────────────────────────────────── */

TEST(IoErrorTest, KindNameCovers) {
  EXPECT_STREQ(kind_name(ErrorKind::NotFound), "NotFound");
  EXPECT_STREQ(kind_name(ErrorKind::ConnectionRefused), "ConnectionRefused");
  EXPECT_STREQ(kind_name(ErrorKind::WouldBlock), "WouldBlock");
  EXPECT_STREQ(kind_name(ErrorKind::ResolveFailed), "ResolveFailed");
  EXPECT_STREQ(kind_name(ErrorKind::Other), "Other");
}

/* ── Simple variant ──────────────────────────────────────────────── */

TEST(IoErrorTest, SimpleConstruction) {
  Error e(ErrorKind::WouldBlock);
  EXPECT_EQ(e.kind(), ErrorKind::WouldBlock);
  EXPECT_TRUE(e.raw_os_error().is_none());
  EXPECT_EQ(e.message(), nullptr);
}

TEST(IoErrorTest, ImplicitFromKind) {
  // Implicit conversion lets you write `return SomeKind;` from
  // a function returning Error.
  auto  make = []() -> Error { return ErrorKind::TimedOut; };
  Error e    = make();
  EXPECT_EQ(e.kind(), ErrorKind::TimedOut);
}

/* ── Os variant ──────────────────────────────────────────────────── */

TEST(IoErrorTest, OsFromErrno) {
  Error e = Error::from_errno(ECONNREFUSED);
  EXPECT_EQ(e.kind(), ErrorKind::ConnectionRefused);
  ASSERT_TRUE(e.raw_os_error().is_some());
  EXPECT_EQ(std::move(e.raw_os_error()).unwrap(), ECONNREFUSED);
}

TEST(IoErrorTest, OsZero) {
  // errno == 0 is degenerate but mustn't collide with the niche.
  Error e = Error::from_errno(0);
  EXPECT_EQ(e.kind(), ErrorKind::Other);
  ASSERT_TRUE(e.raw_os_error().is_some());
  EXPECT_EQ(std::move(e.raw_os_error()).unwrap(), 0);
}

TEST(IoErrorTest, OsNegative) {
  // Some platforms (Windows GetLastError surrogates) use negative codes.
  Error e = Error::from_errno(-42);
  ASSERT_TRUE(e.raw_os_error().is_some());
  EXPECT_EQ(std::move(e.raw_os_error()).unwrap(), -42);
}

/* ── errno → ErrorKind mapping ───────────────────────────────────── */

TEST(IoErrorTest, ErrnoMappingPosixCoverage) {
  EXPECT_EQ(errno_to_kind(ENOENT), ErrorKind::NotFound);
  EXPECT_EQ(errno_to_kind(EACCES), ErrorKind::PermissionDenied);
  EXPECT_EQ(errno_to_kind(EPERM), ErrorKind::PermissionDenied);
  EXPECT_EQ(errno_to_kind(ECONNREFUSED), ErrorKind::ConnectionRefused);
  EXPECT_EQ(errno_to_kind(ECONNRESET), ErrorKind::ConnectionReset);
  EXPECT_EQ(errno_to_kind(ECONNABORTED), ErrorKind::ConnectionAborted);
  EXPECT_EQ(errno_to_kind(ENOTCONN), ErrorKind::NotConnected);
  EXPECT_EQ(errno_to_kind(EADDRINUSE), ErrorKind::AddrInUse);
  EXPECT_EQ(errno_to_kind(EADDRNOTAVAIL), ErrorKind::AddrNotAvailable);
  EXPECT_EQ(errno_to_kind(EHOSTUNREACH), ErrorKind::HostUnreachable);
  EXPECT_EQ(errno_to_kind(EPIPE), ErrorKind::BrokenPipe);
  EXPECT_EQ(errno_to_kind(EAGAIN), ErrorKind::WouldBlock);
  EXPECT_EQ(errno_to_kind(EWOULDBLOCK), ErrorKind::WouldBlock);
  EXPECT_EQ(errno_to_kind(ETIMEDOUT), ErrorKind::TimedOut);
  EXPECT_EQ(errno_to_kind(EINTR), ErrorKind::Interrupted);
  EXPECT_EQ(errno_to_kind(ENOMEM), ErrorKind::OutOfMemory);
  EXPECT_EQ(errno_to_kind(99999), ErrorKind::Other);
}

/* ── SimpleMessage variant ───────────────────────────────────────── */

XPP_IO_DEFINE_MSG(test_closed_msg, NotConnected, "operation on closed socket")

TEST(IoErrorTest, SimpleMessageStatic) {
  Error e = Error::from_static(test_closed_msg());
  EXPECT_EQ(e.kind(), ErrorKind::NotConnected);
  ASSERT_NE(e.message(), nullptr);
  EXPECT_STREQ(e.message(), "operation on closed socket");
  EXPECT_TRUE(e.raw_os_error().is_none());
}

TEST(IoErrorTest, SimpleMessageStableAddress) {
  // Magic-statics: same call yields same pointer within a TU.
  EXPECT_EQ(test_closed_msg(), test_closed_msg());
}

/* ── Custom variant ──────────────────────────────────────────────── */

TEST(IoErrorTest, CustomMessage) {
  Error e = Error::with_message(ErrorKind::InvalidData, "bad header");
  EXPECT_EQ(e.kind(), ErrorKind::InvalidData);
  ASSERT_NE(e.message(), nullptr);
  EXPECT_STREQ(e.message(), "bad header");
  EXPECT_TRUE(e.raw_os_error().is_none());
}

TEST(IoErrorTest, CustomEmptyMessage) {
  Error e = Error::with_message(ErrorKind::InvalidInput, "");
  EXPECT_EQ(e.kind(), ErrorKind::InvalidInput);
  ASSERT_NE(e.message(), nullptr);
  EXPECT_STREQ(e.message(), "");
}

TEST(IoErrorTest, CustomNullMessage) {
  Error e = Error::with_message(ErrorKind::InvalidInput, static_cast<const char *>(nullptr));
  EXPECT_EQ(e.kind(), ErrorKind::InvalidInput);
  // Still NUL-terminated, just empty.
  ASSERT_NE(e.message(), nullptr);
  EXPECT_STREQ(e.message(), "");
}

TEST(IoErrorTest, CustomLongMessage) {
  // Stress allocator with a multi-KB message.
  std::string big(8000, 'x');
  Error       e = Error::with_message(ErrorKind::Other, big.c_str(), big.size());
  EXPECT_EQ(e.kind(), ErrorKind::Other);
  ASSERT_NE(e.message(), nullptr);
  EXPECT_EQ(std::strlen(e.message()), big.size());
  EXPECT_EQ(std::memcmp(e.message(), big.data(), big.size()), 0);
}

/* ── Move semantics ──────────────────────────────────────────────── */

TEST(IoErrorTest, MoveCustomTransfersOwnership) {
  Error a = Error::with_message(ErrorKind::Other, "hello");
  Error b(std::move(a));
  EXPECT_EQ(b.kind(), ErrorKind::Other);
  EXPECT_STREQ(b.message(), "hello");
  // The moved-from `a` is now a harmless Simple(Other).
  EXPECT_EQ(a.kind(), ErrorKind::Other);
  EXPECT_EQ(a.message(), nullptr);
}

TEST(IoErrorTest, MoveAssignDropsPriorCustom) {
  Error a = Error::with_message(ErrorKind::Other, "first");
  Error b = Error::with_message(ErrorKind::InvalidInput, "second");
  a       = std::move(b);
  EXPECT_EQ(a.kind(), ErrorKind::InvalidInput);
  EXPECT_STREQ(a.message(), "second");
}

/* ── to_string ───────────────────────────────────────────────────── */

TEST(IoErrorTest, ToStringSimple) {
  Error e(ErrorKind::WouldBlock);
  auto  s = e.to_string();
  EXPECT_STREQ(s.c_str(), "WouldBlock");
}

TEST(IoErrorTest, ToStringOs) {
  Error e = Error::from_errno(EAGAIN);
  auto  s = e.to_string();
  // Format: "WouldBlock (os error <N>)"
  EXPECT_NE(std::strstr(s.c_str(), "WouldBlock"), nullptr);
  EXPECT_NE(std::strstr(s.c_str(), "os error"), nullptr);
}

TEST(IoErrorTest, ToStringSimpleMessage) {
  Error e = Error::from_static(test_closed_msg());
  auto  s = e.to_string();
  EXPECT_STREQ(s.c_str(), "NotConnected: operation on closed socket");
}

TEST(IoErrorTest, ToStringCustom) {
  Error e = Error::with_message(ErrorKind::InvalidData, "bad header");
  auto  s = e.to_string();
  EXPECT_STREQ(s.c_str(), "InvalidData: bad header");
}

/* ── Option<Error> niche ─────────────────────────────────────────── */

TEST(IoErrorTest, OptionNiche) {
  xpp::Option<Error> none;
  EXPECT_TRUE(none.is_none());

  xpp::Option<Error> some(Error::from_errno(ECONNRESET));
  EXPECT_TRUE(some.is_some());
  EXPECT_EQ(some.kind(), ErrorKind::ConnectionReset);

  Error e = std::move(some).unwrap();
  EXPECT_EQ(e.kind(), ErrorKind::ConnectionReset);
}

TEST(IoErrorTest, OptionMoveCustom) {
  // Confirm the Option niche specialization correctly drops the heap
  // payload when assigned None.
  xpp::Option<Error> opt(Error::with_message(ErrorKind::Other, "leaks?"));
  EXPECT_TRUE(opt.is_some());
  opt = xpp::none;
  EXPECT_TRUE(opt.is_none());
  // No assertion needed — relying on ASAN to catch a leak if drop misfires.
}

/* ── No-allocation paths ─────────────────────────────────────────── */

namespace {

// Track allocations seen by a global new/delete override.  Reentrancy
// is fine because the test body itself doesn't allocate via these
// counters.
struct AllocCounter {
  static int &count() {
    static int c = 0;
    return c;
  }
};

} // namespace

// We can't safely override global operator new in a single test file
// without affecting the rest of the binary.  Instead, exercise a tight
// loop and rely on heap profilers / ASAN if the result ever shifts.
// The structural test here is that `from_errno` and ctor(Kind) compile
// with no observable side effects.
TEST(IoErrorTest, NoAllocConstructorsCompile) {
  for (int i = 0; i < 1000; ++i) {
    Error e1(ErrorKind::WouldBlock);
    (void)e1;
    Error e2 = Error::from_errno(EAGAIN);
    (void)e2;
    Error e3 = Error::from_static(test_closed_msg());
    (void)e3;
  }
}
