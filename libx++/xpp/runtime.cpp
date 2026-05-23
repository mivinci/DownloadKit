/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime.cpp - Runtime lifecycle, worker main loop, work stealing.
 */

#include <xpp/runtime.h>

#include <thread>

extern "C" {
#include <x/base/event.h>
#include <x/base/task.h>
}

namespace xpp {

/* ── Runtime lifecycle ───────────────────────────────────────────── */

Runtime::Runtime(size_t max_workers) : m_active_workers(0) {
  if (max_workers == 0) {
    max_workers = std::thread::hardware_concurrency();
    if (max_workers == 0) max_workers = 4;  // fallback
  }

  m_max_workers = max_workers;
  m_workers = new _::Worker[max_workers];

  m_main_loop = xEventLoopCreate();
  XPP_ASSERT(m_main_loop != nullptr, "Runtime: failed to create main loop");

  // Pre-initialize worker slots (loops created lazily in spawn_worker).
  for (size_t i = 0; i < max_workers; ++i) {
    m_workers[i].id = i;
    m_workers[i].rt = this;
    m_workers[i].loop = nullptr;  // created on first spawn_worker
    m_workers[i].running.store(false, std::memory_order_relaxed);
    m_workers[i].state.store(_::Worker::Idle, std::memory_order_relaxed);
  }

  // Task group sized to max_workers — threads are created lazily by xTaskGroup.
  xTaskGroupConf conf = {max_workers, 0};
  m_group = xTaskGroupCreate(&conf);
  XPP_ASSERT(m_group != nullptr, "Runtime: failed to create task group");

  // No workers started yet — they'll be spawned on demand.
}

Runtime::~Runtime() {
  size_t active = m_active_workers.load(std::memory_order_acquire);
  for (size_t i = 0; i < active; ++i) {
    m_workers[i].running.store(false, std::memory_order_release);
    if (m_workers[i].loop) {
      xEventWake(m_workers[i].loop);
    }
  }

  xTaskGroupDestroy(m_group);

  for (size_t i = 0; i < active; ++i) {
    if (m_workers[i].loop) {
      xEventLoopDestroy(m_workers[i].loop);
    }
  }
  delete[] m_workers;
  xEventLoopDestroy(m_main_loop);
}

/* ── Worker spawning ─────────────────────────────────────────────── */

void Runtime::spawn_worker() {
  size_t idx = m_active_workers.fetch_add(1, std::memory_order_acq_rel);
  if (idx >= m_max_workers) {
    // Raced past the cap — undo.
    m_active_workers.fetch_sub(1, std::memory_order_relaxed);
    return;
  }

  _::Worker &w = m_workers[idx];
  w.loop = xEventLoopCreate();
  XPP_ASSERT(w.loop != nullptr, "Runtime: failed to create worker loop");
  w.running.store(true, std::memory_order_release);
  w.state.store(_::Worker::Idle, std::memory_order_relaxed);

  xTaskSubmit(m_group, worker_main, &w);
}

/* ── Worker main loop ────────────────────────────────────────────── */

void *Runtime::worker_main(void *arg) {
  auto *w = static_cast<_::Worker *>(arg);

  _::tl_current_worker = w;

  WaitScope scope(w->loop);

  while (w->running.load(std::memory_order_acquire)) {
    // 1. Pop from local queue (LIFO, cache-warm).
    _::SpawnTaskBase *task = w->local_queue.pop();

    // 2. If empty, batch-grab from global queue.
    if (!task) {
      w->rt->grab_global(*w, 128);
      task = w->local_queue.pop();
    }

    // 3. If still empty, steal from other workers.
    if (!task) {
      task = w->rt->try_steal(w->id);
    }

    if (task) {
      w->state.store(_::Worker::Running, std::memory_order_relaxed);
      task->execute(scope);
    } else {
      // No work — park until woken.
      w->state.store(_::Worker::Idle, std::memory_order_relaxed);
      xEventWait(w->loop, 100);
    }
  }

  _::tl_current_worker = nullptr;
  return nullptr;
}

/* ── Global queue ────────────────────────────────────────────────── */

void Runtime::push_global(_::SpawnTaskBase *task) {
  m_global_queue.lock()->push_back(task);
}

size_t Runtime::grab_global(_::Worker &w, size_t max_grab) {
  auto guard = m_global_queue.lock();
  size_t n = 0;
  while (n < max_grab && !guard->empty()) {
    _::SpawnTaskBase *task = guard->front();
    guard->pop_front();
    if (!w.local_queue.push(task)) {
      guard->push_front(task);
      break;
    }
    ++n;
  }
  return n;
}

/* ── Wake / spawn decision ───────────────────────────────────────── */

void Runtime::wake_idle_worker() {
  size_t active = m_active_workers.load(std::memory_order_acquire);

  // First, try to wake an existing idle worker.
  for (size_t i = 0; i < active; ++i) {
    if (m_workers[i].state.load(std::memory_order_relaxed) == _::Worker::Idle) {
      xEventWake(m_workers[i].loop);
      return;
    }
  }

  // No idle worker — spawn a new one if under the cap.
  if (active < m_max_workers) {
    spawn_worker();
    return;
  }

  // At cap, all busy — do nothing. Workers will drain the global
  // queue naturally after finishing their current task.
}

/* ── Work stealing ───────────────────────────────────────────────── */

_::SpawnTaskBase *Runtime::try_steal(size_t thief_id) {
  size_t active = m_active_workers.load(std::memory_order_acquire);
  for (size_t i = 1; i < active; ++i) {
    size_t victim = (thief_id + i) % active;
    _::SpawnTaskBase *task = m_workers[victim].local_queue.steal();
    if (task) return task;
  }
  return nullptr;
}

} // namespace xpp
