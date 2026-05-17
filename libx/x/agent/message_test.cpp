/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * message_test.cpp - Unit tests for xai/message.{h,c}
 */

#include <gtest/gtest.h>

extern "C" {
#include <x/agent/message.h>
}

#include <atomic>
#include <cstring>
#include <thread>

/* ── xAgentContentText ───────────────────────────────────────────────────── */

TEST(XaiMessage, ContentTextBasic) {
  xAgentContent c = xAgentContentText("hello");
  EXPECT_EQ(c.type, xAgentContentType_Text);
  ASSERT_NE(c.u.text.text, nullptr);
  EXPECT_STREQ(c.u.text.text, "hello");
  EXPECT_EQ(c.u.text.len, 5u);
}

TEST(XaiMessage, ContentTextEmpty) {
  xAgentContent c = xAgentContentText("");
  EXPECT_EQ(c.type, xAgentContentType_Text);
  EXPECT_EQ(c.u.text.len, 0u);
  /* Empty string is a valid borrowed pointer. */
  ASSERT_NE(c.u.text.text, nullptr);
}

TEST(XaiMessage, ContentTextNullIsAccepted) {
  /* Header calls the arg non-NULL, but the impl must not crash. */
  xAgentContent c = xAgentContentText(nullptr);
  EXPECT_EQ(c.type, xAgentContentType_Text);
  EXPECT_EQ(c.u.text.text, nullptr);
  EXPECT_EQ(c.u.text.len, 0u);
}

TEST(XaiMessage, ContentTextUtf8LengthIsBytes) {
  /* "你好" is 6 bytes in UTF-8; len must report byte count, not chars. */
  const char   *s = "\xe4\xbd\xa0\xe5\xa5\xbd"; /* 你好 */
  xAgentContent c = xAgentContentText(s);
  EXPECT_EQ(c.u.text.len, 6u);
}

/* ── xAgentMessageFromContent ────────────────────────────────────────────── */

TEST(XaiMessage, MessageFromContentCarriesFields) {
  xAgentContent blocks[2];
  blocks[0] = xAgentContentText("a");
  blocks[1] = xAgentContentText("bb");

  xAgentMessage m = xAgentMessageFromContent(xAgentRole_Assistant, blocks, 2);
  EXPECT_EQ(m.role, xAgentRole_Assistant);
  EXPECT_EQ(m.contents, blocks); /* borrows, does not copy */
  EXPECT_EQ(m.n, 2u);
}

TEST(XaiMessage, MessageFromContentEmpty) {
  xAgentMessage m = xAgentMessageFromContent(xAgentRole_System, nullptr, 0);
  EXPECT_EQ(m.role, xAgentRole_System);
  EXPECT_EQ(m.contents, nullptr);
  EXPECT_EQ(m.n, 0u);
}

/* ── xAgentMessageFromText ───────────────────────────────────────────────── */

TEST(XaiMessage, FromTextProducesUserRole) {
  xAgentMessage m = xAgentMessageFromText("ping");
  EXPECT_EQ(m.role, xAgentRole_User);
  ASSERT_EQ(m.n, 1u);
  ASSERT_NE(m.contents, nullptr);
  EXPECT_EQ(m.contents[0].type, xAgentContentType_Text);
  EXPECT_STREQ(m.contents[0].u.text.text, "ping");
}

TEST(XaiMessage, FromTextSecondCallOverwritesTlsSlot) {
  xAgentMessage first = xAgentMessageFromText("first");
  /* Record the slot pointer before the second call. */
  const xAgentContent *first_slot = first.contents;

  xAgentMessage second = xAgentMessageFromText("second");

  /* Same thread → same TLS slot → same pointer. */
  EXPECT_EQ(first_slot, second.contents);
  /* And the slot now reflects "second", not "first" — this is the
   * documented "do not store the returned value across calls" rule. */
  EXPECT_STREQ(second.contents[0].u.text.text, "second");
}

TEST(XaiMessage, FromTextIsThreadLocal) {
  /* Each thread must own its own slot — no cross-thread stomping.
   *
   * Naive approach (spawn → capture → join → compare) does NOT work:
   * after the first thread joins, its TLS storage is reclaimed and
   * the second thread may be handed the same address, giving a
   * false negative.
   *
   * Fix: keep BOTH threads alive simultaneously while capturing
   * their slot pointers, then let them exit. */
  const xAgentContent *a_slot = nullptr;
  const xAgentContent *b_slot = nullptr;
  std::atomic<int>     captured{0};
  std::atomic<bool>    release{false};

  auto body = [&](const char *tag, const xAgentContent **out) {
    xAgentMessage m = xAgentMessageFromText(tag);
    *out            = m.contents;
    captured.fetch_add(1, std::memory_order_acq_rel);
    /* Busy-wait until the test thread has observed both pointers
     * and tells us to exit, so TLS addresses cannot alias. */
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };

  std::thread ta(body, "from-a", &a_slot);
  std::thread tb(body, "from-b", &b_slot);

  /* Spin until both threads have stored their slot pointer. */
  while (captured.load(std::memory_order_acquire) < 2) {
    std::this_thread::yield();
  }

  ASSERT_NE(a_slot, nullptr);
  ASSERT_NE(b_slot, nullptr);
  EXPECT_NE(a_slot, b_slot);

  release.store(true, std::memory_order_release);
  ta.join();
  tb.join();
}
