/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * multi_thread_scheduler.h - Multi-thread work-stealing scheduler.
 *
 * Mirrors Tokio's scheduler::multi_thread. The shared state lives in
 * MultiThreadHandle (level-3, Arc-held): the global injection queue,
 * the per-worker remotes (for stealing / waking), the GlobalSchedule,
 * and the blocking spawner. The owned MultiThreadScheduler holds the
 * worker objects + their thread group and drives shutdown.
 *
 *   spawn(task):
 *     1. on a worker of this runtime  → push to its local queue (fast);
 *        if full, overflow to the global queue + wake an idle worker.
 *     2. otherwise (external thread)   → push to the global queue + wake.
 *
 *   worker main loop:
 *     local pop (LIFO) → batch grab from global → steal (FIFO) → park.
 */

#ifndef XPP_RUNTIME_MULTI_THREAD_SCHEDULER_H
#define XPP_RUNTIME_MULTI_THREAD_SCHEDULER_H

#include <xpp/arc.h>
#include <xpp/box.h>
#include <xpp/promise.h>
#include <xpp/vec.h>

#include <xpp/runtime/blocking_pool.h>
#include <xpp/runtime/context.h>
#include <xpp/runtime/driver.h>
#include <xpp/runtime/queue.h>

#include <xpp/sys/mutex.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <type_traits>
#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {

namespace _ {

// Contract-internal types (PromiseNode, Waker, Schedule, SpawnTaskBase,
// ...) are made visible for a bare `_::` prefix by the using-directive
// in <xpp/runtime/task.h> (pulled in via blocking_pool.h). Repeat it
// here so this header is self-sufficient regardless of include order.
using namespace ::xpp::_;

class Worker;
class MultiThreadHandle;

/* ── WorkerSchedule ──────────────────────────────────────────────── */

/**
 * @brief Schedule attached to tasks claimed by a worker: re-enqueues a
 *        woken task onto that worker's local queue (overflowing to the
 *        global queue if full).
 */
struct WorkerSchedule : Schedule {
  Worker *worker;

  explicit WorkerSchedule(Worker *w) : worker(w) {}

  void schedule(SpawnTaskBase *task) override;
  void yield_now(SpawnTaskBase *task) override;
};

/* ── GlobalSchedule ──────────────────────────────────────────────── */

/**
 * @brief Schedule attached to tasks spawned from outside any worker:
 *        re-enqueues onto the global injection queue + wakes a worker.
 */
class GlobalSchedule : public Schedule {
public:
  explicit GlobalSchedule(MultiThreadHandle *h) : m_handle(h) {}

  void schedule(SpawnTaskBase *task) override;

private:
  MultiThreadHandle *m_handle;
};

/* ── Worker ──────────────────────────────────────────────────────── */

/**
 * @brief Per-worker thread state.
 *
 * Activity (`m_state` Idle/Running) is written ONLY by the worker
 * thread and read by wake_idle_worker() to find a parked worker.
 * `m_shutdown` is a write-once stop flag set by the scheduler dtor.
 * `m_exited` is the worker's exit acknowledgement. Keeping these on
 * separate atomics avoids the race where set_state(Idle) could clobber
 * a concurrent shutdown store.
 *
 * The worker holds an Arc clone of MultiThreadHandle. This is safe (no
 * Arc cycle): the handle owns only non-owning remotes (Worker*), while
 * the Worker objects are owned by MultiThreadScheduler.
 */
class Worker {
public:
  enum State : uint8_t {
    Idle,
    Running
  };

  Worker(size_t id, Arc<MultiThreadHandle> handle, Driver driver)
      : m_id(id), m_handle(std::move(handle)), m_driver(std::move(driver)), m_sched(this) {}

  size_t id() const noexcept {
    return m_id;
  }
  MultiThreadHandle *handle() const noexcept {
    return m_handle.get();
  }
  const Arc<MultiThreadHandle> &handle_arc() const noexcept {
    return m_handle;
  }
  /** @brief This worker's driver (park / unpark + loop handle). */
  Driver &driver() noexcept {
    return m_driver;
  }
  /** @brief Raw event loop bound to this worker. */
  xEventLoop loop() const noexcept {
    return m_driver.handle();
  }
  WorkStealingQueue<SpawnTaskBase *> &local_queue() noexcept {
    return m_local_queue;
  }
  WorkerSchedule &sched() noexcept {
    return m_sched;
  }

  /** Activity state — written only by the worker thread. */
  State state(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return static_cast<State>(m_state.load(order));
  }
  void set_state(State s, std::memory_order order = std::memory_order_relaxed) noexcept {
    m_state.store(static_cast<uint8_t>(s), order);
  }

  /** Shutdown flag — write-once by the scheduler dtor, read by worker. */
  bool alive(std::memory_order order = std::memory_order_acquire) const noexcept {
    return !m_shutdown.load(order);
  }
  void shutdown(std::memory_order order = std::memory_order_release) noexcept {
    m_shutdown.store(true, order);
  }

  /** Exit acknowledgement — set by worker after leaving the main loop. */
  bool exited(std::memory_order order = std::memory_order_acquire) const noexcept {
    return m_exited.load(order);
  }
  void mark_exited() noexcept {
    m_exited.store(true, std::memory_order_release);
  }

  uint32_t tick() const noexcept {
    return m_tick;
  }
  void inc_tick() noexcept {
    ++m_tick;
  }

private:
  size_t                             m_id;
  Arc<MultiThreadHandle>             m_handle;
  Driver                             m_driver;
  WorkStealingQueue<SpawnTaskBase *> m_local_queue;
  WorkerSchedule                     m_sched;
  std::atomic<uint8_t>               m_state{Idle};     // activity (worker-owned)
  std::atomic<bool>                  m_shutdown{false}; // stop signal (dtor-owned)
  std::atomic<bool>                  m_exited{false};   // ack (worker-owned)
  uint32_t                           m_tick{0};
};

/* ── MultiThreadHandle (level-3 shared inner) ────────────────────── */

/**
 * @brief Arc-held shared state for the multi-thread scheduler.
 */
class MultiThreadHandle {
public:
  explicit MultiThreadHandle(Arc<BlockingPool> pool)
      : m_blocking_pool(std::move(pool)), m_global_sched(this) {}

  MultiThreadHandle(const MultiThreadHandle &)            = delete;
  MultiThreadHandle &operator=(const MultiThreadHandle &) = delete;

  /** @brief Spawn an async task; routes onto a worker. */
  template <class Func>
  auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    using T   = typename ReducePromise<decltype(std::declval<Func>()())>::Type;
    auto *task = new SpawnTask<T, typename std::decay<Func>::type>(std::forward<Func>(f));

    Worker *w = Context::current().worker();
    if (w && w->handle() == this) {
      // Fast path: we're on a worker of this runtime.
      task->m_sched = &w->sched();
      if (!w->local_queue().push(task)) {
        push_global(task);
        wake_idle_worker();
      }
    } else {
      // External thread (e.g. block_on): inject + wake.
      task->m_sched = &m_global_sched;
      push_global(task);
      wake_idle_worker();
    }

    return Promise<T>(Own<PromiseNode<T>>(new JoinPromiseNode<T>(task)));
  }

  /** @brief Run a blocking callable on the blocking pool. */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
    return m_blocking_pool->spawn(std::forward<F>(f));
  }

  /* ── Worker registry (built once at startup, then immutable) ───── */

  /** @brief Reserve capacity so add_remote() never reallocates. */
  void reserve_remotes(size_t n) {
    m_remotes = Vec<Worker *>::with_capacity(n);
  }
  /** @brief Register a worker's (non-owning) remote handle. */
  void add_remote(Worker *w) {
    m_remotes.push(w);
  }
  size_t num_workers() const noexcept {
    return m_remotes.len();
  }

  /* ── Scheduling helpers (defined in the .cpp) ──────────────────── */

  /** @brief Push a task onto the global injection queue. */
  void push_global(SpawnTaskBase *task);
  /** @brief Batch-move up to max_grab tasks from global into w's local queue. */
  size_t grab_global(Worker &w, size_t max_grab);
  /** @brief Wake one parked (Idle) worker, if any. */
  void wake_idle_worker();
  /** @brief Try to steal a task from another worker's local queue. */
  SpawnTaskBase *try_steal(size_t thief_id);

private:
  Arc<BlockingPool>                       m_blocking_pool;
  GlobalSchedule                          m_global_sched;
  Vec<Worker *>                           m_remotes;      // non-owning; immutable after startup
  sys::Mutex<std::deque<SpawnTaskBase *>> m_global_queue; // global injection queue
};

} // namespace _

/* ── MultiThreadScheduler (owned executor) ───────────────────────── */

/**
 * @brief Owns the worker objects and their thread group; drives
 *        shutdown. The shared state lives in the Arc-held handle.
 */
class MultiThreadScheduler final {
public:
  MultiThreadScheduler(Arc<_::MultiThreadHandle> handle, size_t nthreads);
  ~MultiThreadScheduler();

  MultiThreadScheduler(const MultiThreadScheduler &)            = delete;
  MultiThreadScheduler &operator=(const MultiThreadScheduler &) = delete;

  MultiThreadScheduler(MultiThreadScheduler &&)            = default;
  MultiThreadScheduler &operator=(MultiThreadScheduler &&) = default;

  const Arc<_::MultiThreadHandle> &handle() const noexcept {
    return m_handle;
  }

private:
  Arc<_::MultiThreadHandle>   m_handle;
  Vec<Box<_::Worker>>         m_workers; // owned worker objects
  Box<void, TaskGroupDeleter> m_group;   // worker thread group (last: joins on destroy)
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_MULTI_THREAD_SCHEDULER_H
