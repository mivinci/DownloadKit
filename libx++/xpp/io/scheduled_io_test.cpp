/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * scheduled_io_test.cpp - Unit tests for xpp::ScheduledIo.
 *
 * Tests use socketpairs with kqueue edge-triggered events (EV_CLEAR).
 * Each "pump()" call delivers at most one event per direction per fd.
 */

#include <gtest/gtest.h>
#include <xpp/io/scheduled_io.h>

#include <atomic>
#include <thread>

extern "C" {
#include <sys/socket.h>
#include <unistd.h>
}

/* ── Mock Schedule ─────────────────────────────────────────────────── */

struct TestSchedule : xpp::_::Schedule {
  std::atomic<bool> &woke;
  explicit TestSchedule(std::atomic<bool> &f) : woke(f) {}
  void schedule(xpp::_::SpawnTaskBase *) override {
    woke.store(true);
  }
};

static xpp::_::Waker test_waker(TestSchedule &sched) {
  return xpp::_::Waker(&sched, nullptr);
}

/* ── Test fixture ──────────────────────────────────────────────────── */

class ScheduledIoTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_loop  = xEventLoopCreate();
    m_guard = new xpp::EnterGuard(nullptr, nullptr, m_loop);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds), 0);
  }

  void TearDown() override {
    if (m_fds[1] >= 0) ::close(m_fds[1]);
    delete m_guard;
    xEventLoopDestroy(m_loop);
  }

  void pump() {
    xEventWait(m_loop, 0);
  }

  xEventLoop       m_loop;
  xpp::EnterGuard *m_guard;
  int              m_fds[2]{-1, -1};
};

/* ── Accessors ─────────────────────────────────────────────────────── */

TEST_F(ScheduledIoTest, FdAccessor) {
  xpp::ScheduledIo sio(m_fds[0]);
  EXPECT_EQ(sio.fd(), m_fds[0]);
  m_fds[0] = -1;
}

TEST_F(ScheduledIoTest, LoopAccessor) {
  xpp::ScheduledIo sio(m_fds[0]);
  EXPECT_EQ(sio.loop(), m_loop);
  m_fds[0] = -1;
}

/* ── poll_read ──────────────────────────────────────────────────────── */

TEST_F(ScheduledIoTest, PollReadNotReady) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_FALSE(sio.poll_read(waker));
  EXPECT_FALSE(woke.load());
}

TEST_F(ScheduledIoTest, PollReadReadyAfterEvent) {
  // Write data → pump delivers readable event → poll_read returns true.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  ASSERT_GE(write(m_fds[1], "x", 1), 0);
  pump();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_TRUE(sio.poll_read(waker));
}

TEST_F(ScheduledIoTest, PollReadClearThenNotReady) {
  // After clear_readable, the readiness bit is gone.  poll_read returns
  // false (the kqueue edge has already fired, no new event until more
  // data arrives).
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  ASSERT_GE(write(m_fds[1], "x", 1), 0);
  pump();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_TRUE(sio.poll_read(waker));
  sio.clear_readable();
  EXPECT_FALSE(sio.poll_read(waker));
}

TEST_F(ScheduledIoTest, PollReadClearThenNewDataReady) {
  // After clear_readable, writing more data triggers a new edge event.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  ASSERT_GE(write(m_fds[1], "x", 1), 0);
  pump();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_TRUE(sio.poll_read(waker));
  sio.clear_readable();
  EXPECT_FALSE(sio.poll_read(waker));

  // Write more data — triggers new readable edge
  ASSERT_GE(write(m_fds[1], "y", 1), 0);
  pump();
  EXPECT_TRUE(sio.poll_read(waker));
}

TEST_F(ScheduledIoTest, PollReadWakesPendingPoller) {
  // poll_read stores waker (not ready).  Writing data triggers event
  // callback which wakes the stored waker.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_FALSE(sio.poll_read(waker));

  std::thread writer([&] { write(m_fds[1], "x", 1); });
  writer.join();
  pump();

  EXPECT_TRUE(woke.load());
}

/* ── poll_write ─────────────────────────────────────────────────────── */

TEST_F(ScheduledIoTest, PollWriteReadyAfterEvent) {
  // kqueue delivers an initial write-readiness event for a fresh socket.
  // After pump, the writable bit is set.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  pump(); // deliver initial write-readiness event

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_TRUE(sio.poll_write(waker));
}

TEST_F(ScheduledIoTest, PollWriteClearThenNotReady) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  pump();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_TRUE(sio.poll_write(waker));
  sio.clear_writable();
  EXPECT_FALSE(sio.poll_write(waker));
}

TEST_F(ScheduledIoTest, PollWriteNotReady) {
  // After clear_writable and no new write events, poll_write is false.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  sio.clear_writable();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_FALSE(sio.poll_write(waker));
}

/* ── close_fd ───────────────────────────────────────────────────────── */

TEST_F(ScheduledIoTest, CloseFdSetsClosed) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  EXPECT_FALSE(sio.is_closed());
  sio.close_fd();
  EXPECT_TRUE(sio.is_closed());
}

TEST_F(ScheduledIoTest, CloseFdIdempotent) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  sio.close_fd();
  sio.close_fd(); // must not crash
  EXPECT_TRUE(sio.is_closed());
}

TEST_F(ScheduledIoTest, CloseFdWakesPollRead) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_FALSE(sio.poll_read(waker));

  sio.close_fd();
  EXPECT_TRUE(woke.load());
  EXPECT_TRUE(sio.poll_read(waker));
}

TEST_F(ScheduledIoTest, CloseFdWakesPollWrite) {
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  sio.clear_writable();

  std::atomic<bool> woke{false};
  TestSchedule      sched(woke);
  auto              waker = test_waker(sched);

  EXPECT_FALSE(sio.poll_write(waker));

  sio.close_fd();
  EXPECT_TRUE(woke.load());
  EXPECT_TRUE(sio.poll_write(waker));
}

/* ── concurrent ─────────────────────────────────────────────────────── */

TEST_F(ScheduledIoTest, ConcurrentPollAndEvent) {
  // Thread A polls, thread B writes + pumps.  The poller should see
  // readiness via the first check or the double-check under lock.
  xpp::ScheduledIo sio(m_fds[0]);
  m_fds[0] = -1;

  std::atomic<bool> poll_result{false};
  std::atomic<bool> ready{false};

  std::thread poller([&] {
    while (!ready.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::atomic<bool> woke{false};
    TestSchedule      sched(woke);
    auto              waker = test_waker(sched);
    poll_result.store(sio.poll_read(waker));
  });

  std::thread writer([&] {
    write(m_fds[1], "x", 1);
    pump();
    ready.store(true, std::memory_order_release);
  });

  poller.join();
  writer.join();

  EXPECT_TRUE(poll_result.load());
}
