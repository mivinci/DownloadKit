/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * current_thread_scheduler.h - Single-threaded executor.
 *
 * Mirrors Tokio's scheduler::current_thread. There are no worker
 * threads: spawned tasks are enqueued onto a run queue owned by the
 * Arc-held CurrentThreadHandle and executed on the thread that calls
 * Runtime::block_on, interleaved with the main future and the event
 * loop (see the current-thread drive loop in scheduler.h).
 */

#ifndef XPP_RUNTIME_CURRENT_THREAD_SCHEDULER_H
#define XPP_RUNTIME_CURRENT_THREAD_SCHEDULER_H

#include <xpp/arc.h>
#include <xpp/panic.h>
#include <xpp/promise.h>

#include <xpp/runtime/blocking_pool.h>

#include <xpp/sys/mutex.h>

#include <atomic>
#include <deque>
#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {

namespace _ {

// Contract-internal types (PromiseNode, Waker, Schedule, SpawnTaskBase,
// ...) are visible for a bare `_::` prefix via the using-directive in
// <xpp/runtime/task.h> (pulled in through blocking_pool.h).

class CurrentThreadHandle;

/* ── CurrentThreadSchedule ───────────────────────────────────────── */

/**
 * @brief Schedule for tasks owned by the current-thread runtime:
 *        (re)enqueues a woken task onto the run queue and nudges the
 *        block_on loop in case it is parked.
 */
struct CurrentThreadSchedule : Schedule {
  CurrentThreadHandle *handle;

  explicit CurrentThreadSchedule(CurrentThreadHandle *h) : handle(h) {}

  void schedule(SpawnTaskBase *task) override;
};

/* ── CurrentThreadHandle (level-3 shared inner) ──────────────────── */

/**
 * @brief Arc-held shared state for the current-thread scheduler.
 *
 * Holds the run queue of spawned tasks, the blocking spawner, and the
 * loop pointer used to unpark block_on when a task is enqueued from a
 * different thread (e.g. a spawn_blocking completion).
 */
class CurrentThreadHandle {
public:
  explicit CurrentThreadHandle(Arc<BlockingPool> pool);

  CurrentThreadHandle(const CurrentThreadHandle &)            = delete;
  CurrentThreadHandle &operator=(const CurrentThreadHandle &) = delete;

  /** @brief Spawn an async task onto the run queue. */
  template <class Func>
  auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    using T    = typename ReducePromise<decltype(std::declval<Func>()())>::Type;
    auto *task = new SpawnTask<T, typename std::decay<Func>::type>(std::forward<Func>(f));
    task->m_sched = &m_schedule;
    push_task(task);
    wake();
    return Promise<T>(Own<PromiseNode<T>>(new JoinPromiseNode<T>(task)));
  }

  /** @brief Run a blocking callable on the blocking pool. */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
    return m_blocking_pool->spawn(std::forward<F>(f));
  }

  /* ── Run queue (drained by the block_on drive loop) ────────────── */

  void           push_task(SpawnTaskBase *task);
  SpawnTaskBase *pop_task();
  bool           has_tasks();

  CurrentThreadSchedule &schedule() noexcept {
    return m_schedule;
  }

  /* ── Park / unpark plumbing ────────────────────────────────────── */

  /** @brief Bind the block_on loop for the duration of a drive. */
  void set_loop(xEventLoop loop) noexcept;
  void clear_loop() noexcept;
  /** @brief Nudge the block_on loop, if one is parked. */
  void wake() noexcept;

private:
  Arc<BlockingPool>                       m_blocking_pool;
  CurrentThreadSchedule                   m_schedule;
  sys::Mutex<std::deque<SpawnTaskBase *>> m_run_queue;
  std::atomic<xEventLoop>                 m_loop{nullptr};
};

} // namespace _

/* ── CurrentThreadScheduler (owned executor) ─────────────────────── */

class CurrentThreadScheduler final {
public:
  explicit CurrentThreadScheduler(Arc<_::CurrentThreadHandle> handle)
      : m_handle(std::move(handle)) {}

  const Arc<_::CurrentThreadHandle> &handle() const noexcept {
    return m_handle;
  }

private:
  Arc<_::CurrentThreadHandle> m_handle;
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_CURRENT_THREAD_SCHEDULER_H
