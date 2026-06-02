/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * multi_thread_scheduler.cpp - Worker main loop + work stealing.
 */

#include <xpp/runtime/multi_thread_scheduler.h>

#include <xpp/runtime/scheduler.h> // _::SchedulerHandle (installed into Context)

#include <unistd.h> // usleep

extern "C" {
#include <x/base/event.h>
#include <x/base/task.h>
}

namespace xpp {
namespace runtime {

namespace {

/// Worker threads run permanently (worker_main never returns until
/// shutdown), so the worker group needs exactly one slot per worker.
void *create_worker_group(size_t nthreads) {
  xTaskGroupConf conf = {nthreads, 0};
  return xTaskGroupCreate(&conf);
}

/* ── Worker main loop ────────────────────────────────────────────── */

void *worker_main(void *arg) {
  auto *w = static_cast<_::Worker *>(arg);

  // Install this thread's runtime context: a SchedulerHandle pointing at
  // our runtime (so xpp::spawn() routes here), the worker (fast path),
  // and the worker's event loop (driver + park). The SchedulerHandle
  // lives on the stack and outlives the guard's Context pointer.
  _::SchedulerHandle sched(w->handle_arc().clone());
  SetContextGuard    guard(&sched, w, w->loop(), nullptr);

  _::MultiThreadHandle *h = w->handle();

  while (w->alive(std::memory_order_acquire)) {
    _::SpawnTaskBase *task = w->local_queue().pop();

    // Periodically poll the global queue even when local is non-empty,
    // to avoid starving tasks injected from external threads.
    if (!task || (w->tick() & 0x3F) == 0) {
      h->grab_global(*w, 128);
      if (!task) task = w->local_queue().pop();
    }

    if (!task) task = h->try_steal(w->id());

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
          uint8_t prev =
            task->state.exchange(_::SpawnTaskBase::Completed, std::memory_order_acq_rel);
          task->wake_join();
          if (prev == _::SpawnTaskBase::Detached) {
            xEventLoopPost(
              w->loop(), [](void *a) { delete static_cast<_::SpawnTaskBase *>(a); }, task);
          }
        } else {
          task->state.store(_::SpawnTaskBase::Pending, std::memory_order_release);
        }
      }
      w->inc_tick();
    } else {
      w->set_state(_::Worker::Idle, std::memory_order_relaxed);
      w->driver().park_timeout(100);
    }
  }

  w->mark_exited();
  return nullptr;
}

} // namespace

/* ── WorkerSchedule / GlobalSchedule ─────────────────────────────── */

namespace _ {

void WorkerSchedule::schedule(SpawnTaskBase *task) {
  if (worker->local_queue().push(task)) return;
  worker->handle()->push_global(task);
  worker->handle()->wake_idle_worker();
}

void WorkerSchedule::yield_now(SpawnTaskBase *task) {
  schedule(task);
}

void GlobalSchedule::schedule(SpawnTaskBase *task) {
  m_handle->push_global(task);
  m_handle->wake_idle_worker();
}

/* ── MultiThreadHandle scheduling helpers ────────────────────────── */

void MultiThreadHandle::push_global(SpawnTaskBase *task) {
  m_global_queue.lock()->push_back(task);
}

size_t MultiThreadHandle::grab_global(Worker &w, size_t max_grab) {
  auto   guard = m_global_queue.lock();
  size_t n     = 0;
  while (n < max_grab && !guard->empty()) {
    SpawnTaskBase *task = guard->front();
    guard->pop_front();
    if (!w.local_queue().push(task)) {
      guard->push_front(task);
      break;
    }
    ++n;
  }
  return n;
}

void MultiThreadHandle::wake_idle_worker() {
  size_t n = m_remotes.len();
  for (size_t i = 0; i < n; ++i) {
    if (m_remotes[i]->state(std::memory_order_relaxed) == Worker::Idle) {
      m_remotes[i]->driver().unpark();
      return;
    }
  }
}

SpawnTaskBase *MultiThreadHandle::try_steal(size_t thief_id) {
  size_t active = m_remotes.len();
  for (size_t i = 1; i < active; ++i) {
    size_t         victim = (thief_id + i) % active;
    SpawnTaskBase *task   = m_remotes[victim]->local_queue().steal();
    if (task) return task;
  }
  return nullptr;
}

} // namespace _

/* ── MultiThreadScheduler lifecycle ──────────────────────────────── */

MultiThreadScheduler::MultiThreadScheduler(Arc<_::MultiThreadHandle> handle, size_t nthreads)
    : m_handle(std::move(handle)), m_workers(Vec<Box<_::Worker>>::with_capacity(nthreads)),
      m_group(Box<void, TaskGroupDeleter>::from_raw(create_worker_group(nthreads))) {
  XPP_ASSERT(m_group.get() != nullptr, "MultiThreadScheduler: failed to create worker group");

  m_handle->reserve_remotes(nthreads);

  // Phase 1: create workers and register remotes. Single-threaded — no
  // worker is running yet, so the remote registry is built race-free.
  for (size_t i = 0; i < nthreads; ++i) {
    auto worker =
      Box<_::Worker>::from_raw(new _::Worker(i, m_handle.clone(), Driver::create()));
    m_handle->add_remote(worker.get());
    m_workers.push(std::move(worker));
  }

  // Phase 2: launch threads. The remote registry is now complete and
  // immutable, so stealing/waking across workers is safe.
  for (size_t i = 0; i < nthreads; ++i) {
    xTaskSubmit(m_group.get(), worker_main, m_workers[i].get());
  }
}

MultiThreadScheduler::~MultiThreadScheduler() {
  if (m_workers.len() == 0) return; // moved-from husk: nothing to shut down

  size_t n = m_workers.len();
  // Signal every worker to stop and nudge it out of park.
  for (size_t i = 0; i < n; ++i) {
    m_workers[i]->shutdown(std::memory_order_release);
    m_workers[i]->driver().unpark();
  }
  // Wait until each worker has acknowledged exit, re-nudging in case a
  // wake raced the park. After this, m_group destruction (joins the
  // threads) and m_workers destruction are both safe.
  for (size_t i = 0; i < n; ++i) {
    while (!m_workers[i]->exited(std::memory_order_acquire)) {
      m_workers[i]->driver().unpark();
      usleep(50);
    }
  }
}

} // namespace runtime
} // namespace xpp
