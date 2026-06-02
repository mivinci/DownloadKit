/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * scheduler.h - Scheduler (owned executor) + SchedulerHandle (shared).
 *
 * Three-tier handle model, mirroring Tokio:
 *
 *   Scheduler        (owned by Runtime)        ~ tokio runtime::Scheduler
 *     Variant<CurrentThreadScheduler, MultiThreadScheduler>
 *
 *   _::SchedulerHandle (shared, cloneable)     ~ tokio scheduler::Handle
 *     Variant<Arc<CurrentThreadHandle>, Arc<MultiThreadHandle>>
 *     Stored in the thread-local Context; used by xpp::spawn / spawn_blocking.
 *
 *   _::MultiThreadHandle / _::CurrentThreadHandle  ~ tokio mt/ct::Handle
 *     The Arc-held shared state each flavor exposes.
 */

#ifndef XPP_RUNTIME_SCHEDULER_H
#define XPP_RUNTIME_SCHEDULER_H

#include <xpp/arc.h>
#include <xpp/panic.h>
#include <xpp/promise.h>
#include <xpp/variant.h>

#include <xpp/runtime/blocking_pool.h>
#include <xpp/runtime/current_thread_scheduler.h>
#include <xpp/runtime/driver.h>
#include <xpp/runtime/multi_thread_scheduler.h>

#include <cstdint>
#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {

namespace _ {

// Make contract-internal types (PromiseNode, Waker, SyncWaitSchedule,
// SpawnTaskBase) available with a bare `_::` prefix.
using namespace ::xpp::_;

/* ── SchedulerHandle (level-2: flavor dispatch) ──────────────────── */

/**
 * @brief Cloneable handle that dispatches spawn / spawn_blocking to the
 *        active flavor's level-3 handle.
 *
 * Held by value in the thread-local Context (cheap: one Arc). This is
 * what xpp::spawn() / xpp::spawn_blocking() route through.
 */
class SchedulerHandle {
public:
  enum Kind {
    CurrentThread,
    MultiThread,
  };

  explicit SchedulerHandle(Arc<CurrentThreadHandle> h) : m_inner(std::move(h)) {}
  explicit SchedulerHandle(Arc<MultiThreadHandle> h) : m_inner(std::move(h)) {}

  /** @brief Spawn an async task onto the active backend. */
  template <class Func>
  auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    switch (m_inner.index()) {
    case CurrentThread:
      return m_inner.template get_unchecked<CurrentThread>()->spawn(std::forward<Func>(f));
    case MultiThread:
      return m_inner.template get_unchecked<MultiThread>()->spawn(std::forward<Func>(f));
    default:
      XPP_PANIC("unreachable");
    }
  }

  /** @brief Run a blocking callable on the active backend's blocking pool. */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
    switch (m_inner.index()) {
    case CurrentThread:
      return m_inner.template get_unchecked<CurrentThread>()->spawn_blocking(std::forward<F>(f));
    case MultiThread:
      return m_inner.template get_unchecked<MultiThread>()->spawn_blocking(std::forward<F>(f));
    default:
      XPP_PANIC("unreachable");
    }
  }

private:
  Variant<Arc<CurrentThreadHandle>, Arc<MultiThreadHandle>> m_inner;
};

/* ── block_on drive helpers ──────────────────────────────────────── */

/**
 * @brief Execute one spawned task up to its next poll point on the
 *        calling thread (current-thread flavor).
 *
 * Claims the task (Pending/Detached → Running), polls it once, and on
 * completion wakes its joiner; a detached task self-deletes (posted to
 * the loop so deletion happens outside the run-queue drain).
 */
inline void run_local_task(CurrentThreadHandle *h, xEventLoop loop, SpawnTaskBase *task) {
  uint8_t expected = SpawnTaskBase::Pending;
  if (!task->state.compare_exchange_strong(expected, SpawnTaskBase::Running,
                                           std::memory_order_acq_rel)) {
    expected = SpawnTaskBase::Detached;
    if (!task->state.compare_exchange_strong(expected, SpawnTaskBase::Running,
                                             std::memory_order_acq_rel)) {
      return; // already claimed elsewhere
    }
  }

  Waker waker(&h->schedule(), task);
  if (task->poll(waker)) {
    uint8_t prev = task->state.exchange(SpawnTaskBase::Completed, std::memory_order_acq_rel);
    task->wake_join();
    if (prev == SpawnTaskBase::Detached) {
      xEventLoopPost(
        loop, [](void *a) { delete static_cast<SpawnTaskBase *>(a); }, task);
    }
  } else {
    task->state.store(SpawnTaskBase::Pending, std::memory_order_release);
  }
}

/**
 * @brief Multi-thread drive: spawned tasks run on workers, so block_on
 *        only polls the main future and parks the calling thread.
 */
template <class Node> void drive_parking(Driver &driver, Node *node) {
  bool             fired = false;
  SyncWaitSchedule sync(&fired, driver.handle());
  while (!node->poll(Waker(&sync, nullptr))) {
    while (!fired)
      driver.park();
    fired = false;
  }
}

/**
 * @brief Current-thread drive: the calling thread runs both the spawned
 *        tasks (drained from the run queue) and the main future,
 *        interleaved with the event loop, parking only when idle.
 */
template <class Node> void drive_current_thread(CurrentThreadHandle *h, Driver &driver, Node *node) {
  h->set_loop(driver.handle());
  bool             fired = false;
  SyncWaitSchedule sync(&fired, driver.handle());
  for (;;) {
    while (SpawnTaskBase *task = h->pop_task())
      run_local_task(h, driver.handle(), task);
    if (node->poll(Waker(&sync, nullptr))) break;
    if (h->has_tasks()) continue; // tasks (re)queued during poll: keep working
    while (!fired && !h->has_tasks())
      driver.park();
    fired = false;
  }
  h->clear_loop();
}

} // namespace _

/* ── Scheduler (level-1: owned executor) ─────────────────────────── */

/**
 * @brief The executor owned by Runtime. Owns worker-thread lifetime and
 *        shutdown; shares run-time state with SchedulerHandle via Arc.
 */
class Scheduler {
public:
  Scheduler(const Scheduler &)            = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&)                 = default;
  Scheduler &operator=(Scheduler &&)      = default;

  static Scheduler current_thread(Arc<BlockingPool> pool) {
    auto handle = Arc<_::CurrentThreadHandle>::make(std::move(pool));
    return Scheduler(Impl(CurrentThreadScheduler(std::move(handle))));
  }

  static Scheduler multi_thread(Arc<BlockingPool> pool, size_t nthreads) {
    auto handle = Arc<_::MultiThreadHandle>::make(std::move(pool));
    return Scheduler(Impl(MultiThreadScheduler(std::move(handle), nthreads)));
  }

  /** @brief A cloneable SchedulerHandle sharing this executor's state. */
  _::SchedulerHandle handle() const {
    switch (m_impl.index()) {
    case CurrentThread:
      return _::SchedulerHandle(m_impl.template get_unchecked<CurrentThread>().handle());
    case MultiThread:
      return _::SchedulerHandle(m_impl.template get_unchecked<MultiThread>().handle());
    default:
      XPP_PANIC("unreachable");
    }
  }

  /**
   * @brief Drive a promise node to completion on the calling thread,
   *        using the backend-appropriate strategy.
   *
   * Multi-thread: poll + park (workers execute spawned tasks).
   * Current-thread: drain the run queue + poll the main future inline.
   */
  template <class Node> void block_on_drive(Driver &driver, Node *node) {
    switch (m_impl.index()) {
    case CurrentThread:
      _::drive_current_thread(m_impl.template get_unchecked<CurrentThread>().handle().get(), driver,
                              node);
      break;
    case MultiThread:
      _::drive_parking(driver, node);
      break;
    default:
      XPP_PANIC("unreachable");
    }
  }

private:
  enum Kind {
    CurrentThread,
    MultiThread,
  };
  using Impl = Variant<CurrentThreadScheduler, MultiThreadScheduler>;
  explicit Scheduler(Impl impl) : m_impl(std::move(impl)) {}

  Impl m_impl;
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_SCHEDULER_H
