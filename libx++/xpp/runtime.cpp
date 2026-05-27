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

#include <unistd.h>

namespace xpp {

namespace {

size_t resolve_max_workers(size_t n) {
  if (n == 0) n = std::thread::hardware_concurrency();
  return n ? n : 4;
}

xTaskGroup create_task_group(size_t n) {
  xTaskGroupConf conf = {n, 0};
  return xTaskGroupCreate(&conf);
}

} // namespace

/* ── Thread-local state (single definition) ─────────────────────── */

namespace _ {

static thread_local Context tl_context{nullptr, nullptr, nullptr};

Context &current_context() {
  return tl_context;
}

} // namespace _

/* ── EnterGuard ──────────────────────────────────────────────────── */

EnterGuard::EnterGuard(Runtime *rt, _::Worker *w, xEventLoop loop) : m_loop(loop) {
  XPP_ASSERT(_::tl_context.runtime == nullptr,
             "EnterGuard: thread already inside a runtime");
  _::tl_context = {rt, w, loop};
}

EnterGuard::~EnterGuard() {
  _::tl_context = {nullptr, nullptr, nullptr};
}

namespace _ {

void WorkerSchedule::schedule(SpawnTaskBase *task) {
  if (worker->local_queue().push(task)) return;
  worker->rt()->push_global(task);
  worker->rt()->wake_idle_worker();
}

void WorkerSchedule::yield_now(SpawnTaskBase *task) {
  schedule(task);
}

} // namespace _

/* ── GlobalSchedule ──────────────────────────────────────────────── */

void _::GlobalSchedule::schedule(SpawnTaskBase *task) {
  m_rt->push_global(task);
  m_rt->wake_idle_worker();
}

/* ── Runtime lifecycle ───────────────────────────────────────────── */

Runtime::Runtime(size_t num_workers)
    : m_num_workers(resolve_max_workers(num_workers)),
      m_workers(Vec<Box<_::Worker>>::with_capacity(m_num_workers)),
      m_main_loop(Box<void, EventLoopDeleter>::from_raw(xEventLoopCreate())),
      m_global_sched(this),
      m_group(Box<void, xpp::TaskGroupDeleter>::from_raw(create_task_group(m_num_workers))) {
  XPP_ASSERT(m_main_loop.get() != nullptr, "Runtime: failed to create main loop");
  XPP_ASSERT(m_group.get() != nullptr, "Runtime: failed to create task group");

  for (size_t i = 0; i < m_num_workers; ++i) {
    auto worker = Box<_::Worker>::from_raw(
      new _::Worker(i, this, Box<void, EventLoopDeleter>::from_raw(xEventLoopCreate())));
    _::Worker *w = worker.get();
    m_workers.push(std::move(worker));
    xTaskSubmit(m_group.get(), worker_main, w);
  }
}

Runtime::~Runtime() {
  // Signal all workers to stop.
  for (size_t i = 0; i < m_num_workers; ++i) {
    m_workers[i]->shutdown(std::memory_order_release);
    xEventWake(m_workers[i]->loop().get());
  }
  // Spin-wake until all workers have acknowledged exit.
  for (size_t i = 0; i < m_num_workers; ++i) {
    while (!m_workers[i]->exited(std::memory_order_acquire)) {
      xEventWake(m_workers[i]->loop().get());
      usleep(50);
    }
  }
}

/* ── Worker main loop ────────────────────────────────────────────── */

void *Runtime::worker_main(void *arg) {
  auto *w = static_cast<_::Worker *>(arg);

  EnterGuard guard(w->rt(), w, w->loop().get());

  while (w->alive(std::memory_order_acquire)) {
    _::SpawnTaskBase *task = w->local_queue().pop();

    // Periodically check the global queue even if local is non-empty,
    // to prevent starvation of tasks spawned from external threads.
    if (!task || (w->tick() & 0x3F) == 0) {
      w->rt()->grab_global(*w, 128);
      if (!task) task = w->local_queue().pop();
    }

    if (!task) {
      task = w->rt()->try_steal(w->id());
    }

    if (task) {
      uint8_t expected = _::SpawnTaskBase::Pending;
      if (task->state.compare_exchange_strong(expected, _::SpawnTaskBase::Running,
                                              std::memory_order_acq_rel)) {
        goto claimed;
      }
      expected = _::SpawnTaskBase::Detached;
      if (task->state.compare_exchange_strong(expected, _::SpawnTaskBase::Running,
                                              std::memory_order_acq_rel)) {
        goto claimed;
      }
      continue;

    claimed:
      w->set_state(_::Worker::Running, std::memory_order_relaxed);
      {
        _::Waker waker(&w->sched(), task);
        if (task->poll(waker)) {
          uint8_t prev = task->state.exchange(_::SpawnTaskBase::Completed,
                                              std::memory_order_acq_rel);
          task->wake_join();
          if (prev == _::SpawnTaskBase::Detached) {
            xEventLoopPost(w->loop().get(),
                           [](void *arg) { delete static_cast<_::SpawnTaskBase *>(arg); },
                           task);
          }
        } else {
          task->state.store(_::SpawnTaskBase::Pending, std::memory_order_release);
        }
      }
      w->inc_tick();
      task = nullptr;
    } else {
      w->set_state(_::Worker::Idle, std::memory_order_relaxed);
      xEventWait(w->loop().get(), 100);
    }
  }

  w->mark_exited();
  return nullptr;
}

/* ── Global queue ────────────────────────────────────────────────── */

void Runtime::push_global(_::SpawnTaskBase *task) {
  m_global_queue.lock()->push_back(task);
}

size_t Runtime::grab_global(_::Worker &w, size_t max_grab) {
  auto   guard = m_global_queue.lock();
  size_t n     = 0;
  while (n < max_grab && !guard->empty()) {
    _::SpawnTaskBase *task = guard->front();
    guard->pop_front();
    if (!w.local_queue().push(task)) {
      guard->push_front(task);
      break;
    }
    ++n;
  }
  return n;
}

/* ── Wake decision ────────────────────────────────────────────────── */

void Runtime::wake_idle_worker() {
  for (size_t i = 0; i < m_num_workers; ++i) {
    if (m_workers[i]->state(std::memory_order_relaxed) == _::Worker::Idle) {
      xEventWake(m_workers[i]->loop().get());
      return;
    }
  }
}

/* ── Work stealing ───────────────────────────────────────────────── */

_::SpawnTaskBase *Runtime::try_steal(size_t thief_id) {
  size_t active = m_workers.len();
  for (size_t i = 1; i < active; ++i) {
    size_t            victim = (thief_id + i) % active;
    _::SpawnTaskBase *task   = m_workers[victim]->local_queue().steal();
    if (task) return task;
  }
  return nullptr;
}

xEventLoop current_event_loop() {
  return _::current_context().loop;
}

} // namespace xpp
