/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.cpp - Non-template Promise infrastructure:
 *   - WaitScope (thread_local registration)
 *   - Event::arm (xEventLoopPost bridge)
 */

#include <xpp/promise.h>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

/* ── WaitScope ────────────────────────────────────────────────────── */

static thread_local xEventLoop tl_current_loop = nullptr;

WaitScope::WaitScope(xEventLoop loop) : m_loop(loop) {
  XPP_ASSERT(loop != nullptr, "WaitScope: loop must not be null");
  XPP_ASSERT(tl_current_loop == nullptr, "WaitScope: this thread already has an active WaitScope");
  tl_current_loop = loop;
}

WaitScope::~WaitScope() {
  tl_current_loop = nullptr;
}

xEventLoop WaitScope::current_loop() {
  return tl_current_loop;
}

/* ── Event::arm ───────────────────────────────────────────────────── */

namespace _ {

static void event_post_callback(void *arg) {
  auto *event = static_cast<Event *>(arg);
  event->fire();
}

void Event::arm() {
  if (m_fired) return;
  xEventLoop loop = tl_current_loop;
  if (loop) {
    xEventLoopPost(loop, event_post_callback, this);
  } else {
    fire();
  }
}

} // namespace _
} // namespace xpp
