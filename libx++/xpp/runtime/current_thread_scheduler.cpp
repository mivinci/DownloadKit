/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * current_thread_scheduler.cpp - Out-of-line members of the
 * single-threaded executor (run queue + park/unpark plumbing). The
 * spawn/spawn_blocking templates stay in the header.
 */

#include <xpp/runtime/current_thread_scheduler.h>

#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {
namespace _ {

CurrentThreadHandle::CurrentThreadHandle(Arc<BlockingPool> pool)
    : m_blocking_pool(std::move(pool)), m_schedule(this) {}

/* ── Run queue ─────────────────────────────────────────────────────── */

void CurrentThreadHandle::push_task(SpawnTaskBase *task) {
  m_run_queue.lock()->push_back(task);
}

SpawnTaskBase *CurrentThreadHandle::pop_task() {
  auto guard = m_run_queue.lock();
  if (guard->empty()) return nullptr;
  SpawnTaskBase *task = guard->front();
  guard->pop_front();
  return task;
}

bool CurrentThreadHandle::has_tasks() {
  return !m_run_queue.lock()->empty();
}

/* ── Park / unpark plumbing ────────────────────────────────────────── */

void CurrentThreadHandle::set_loop(xEventLoop loop) noexcept {
  m_loop.store(loop, std::memory_order_release);
}

void CurrentThreadHandle::clear_loop() noexcept {
  m_loop.store(nullptr, std::memory_order_release);
}

void CurrentThreadHandle::wake() noexcept {
  xEventLoop loop = m_loop.load(std::memory_order_acquire);
  if (loop) xEventWake(loop);
}

/* ── CurrentThreadSchedule ─────────────────────────────────────────── */

void CurrentThreadSchedule::schedule(SpawnTaskBase *task) {
  handle->push_task(task);
  handle->wake();
}

} // namespace _
} // namespace runtime
} // namespace xpp
