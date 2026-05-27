/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime.h - Multi-threaded async runtime with work stealing.
 *
 * Provides a Tokio/Go-style Runtime that manages a dynamically-scaled
 * pool of worker threads. Tasks (Promises) are spawned onto workers
 * and driven to completion. Idle workers steal tasks from busy workers'
 * local queues.
 *
 * Architecture:
 *   Runtime
 *    ├── Worker[0..N]: pre-created at construction, capped at CPU core count
 *    ├── Global queue: overflow + external spawn entry point
 *    └── Per-worker: xEventLoop + WorkStealingQueue (local runqueue)
 *
 * Scheduling (Go-style):
 *   spawn():
 *     1. If caller is a worker thread → push to local queue (fast path)
 *     2. Otherwise → push to global queue + wake an idle worker
 *
 *   Worker main loop:
 *     1. pop from local queue (LIFO, cache-warm)
 *     2. if empty, batch-grab from global queue
 *     3. if still empty, steal from other workers (FIFO)
 *     4. if no work found, park (xEventWait)
 *     5. poll task: task.poll(waker) drives the promise forward
 *     6. on completion, wake the JoinHandle
 *
 *   Worker scaling:
 *     - All workers pre-created at Runtime construction
 *     - Cap = hardware concurrency (or user-specified)
 *     - Idle workers park cheaply (xEventWait)
 *
 * Design inspired by Tokio (Rust) and Go runtime.
 */

#ifndef XPP_RUNTIME_H
#define XPP_RUNTIME_H

#include <xpp/box.h>
#include <xpp/compiler.h>
#include <xpp/own.h>
#include <xpp/promise.h>
#include <xpp/sys/mutex.h>
#include <xpp/vec.h>

#include <atomic>
#include <deque>

extern "C" {
#include <x/base/event.h>
#include <x/base/task.h>
}

namespace xpp {

struct EventLoopDeleter {
  void operator()(void *p) const noexcept {
    xEventLoopDestroy(p);
  }
};

struct TaskGroupDeleter {
  void operator()(void *p) const noexcept {
    xTaskGroupDestroy(p);
  }
};

} // namespace xpp

#if XPP_HAS_COROUTINES
#include <coroutine>
#endif

namespace xpp {

class Runtime;
class EnterGuard;
template <class T> class JoinHandle;

namespace _ {

/* ── WorkStealingQueue ───────────────────────────────────────────── */

/**
 * @brief Fixed-size single-producer multi-consumer ring buffer.
 *
 * Modeled after Go's P-local runqueue: a power-of-two array with
 * atomic head/tail indices.
 *
 *   Owner (the worker thread): push() / pop() at head (LIFO).
 *   Thieves (other workers):   steal() from tail (FIFO).
 *
 * If full, push() returns false — caller overflows to global queue.
 *
 * @tparam T  Element type (pointer-sized).
 * @tparam N  Capacity. Must be a power of two.
 */
template <class T, size_t N = 256> class WorkStealingQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  static constexpr uint32_t kMask = static_cast<uint32_t>(N - 1);

public:
  WorkStealingQueue() : m_head(0), m_tail(0) {}

  /**
   * @brief Push an item. Owner thread only.
   * @return false if full.
   */
  bool push(T item) {
    uint32_t h = m_head.load(std::memory_order_relaxed);
    uint32_t t = m_tail.load(std::memory_order_acquire);
    if (h - t >= N) return false;
    m_slots[h & kMask] = item;
    m_head.store(h + 1, std::memory_order_release);
    return true;
  }

  /**
   * @brief Pop an item. Owner thread only. LIFO order.
   * @return The item, or nullptr if empty.
   */
  T pop() {
    uint32_t h = m_head.load(std::memory_order_relaxed);
    uint32_t t = m_tail.load(std::memory_order_acquire);
    if (h == t) return nullptr; // empty
    --h;
    m_head.store(h, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    t = m_tail.load(std::memory_order_relaxed);
    if (t <= h) {
      return m_slots[h & kMask]; // no conflict
    }
    // Conflict with stealer on last element.
    if (t == h) {
      if (m_tail.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
        m_head.store(h + 1, std::memory_order_relaxed);
        return m_slots[h & kMask]; // we won
      }
    }
    // Lost the race — queue is empty.
    m_head.store(t, std::memory_order_relaxed);
    return nullptr;
  }

  /**
   * @brief Steal an item. Any thread. FIFO order (oldest first).
   * @return The item, or nullptr if empty or lost CAS race.
   */
  T steal() {
    uint32_t t = m_tail.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint32_t h = m_head.load(std::memory_order_acquire);
    if (t >= h) return nullptr;
    T item = m_slots[t & kMask];
    if (m_tail.compare_exchange_weak(t, t + 1, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      return item;
    }
    return nullptr;
  }

  bool empty() const {
    return m_tail.load(std::memory_order_acquire) >= m_head.load(std::memory_order_acquire);
  }

private:
  std::atomic<uint32_t> m_head; // owner writes, thieves read
  std::atomic<uint32_t> m_tail; // thieves CAS, owner reads
  T                     m_slots[N];
};

/* ── SpawnTask<T> ────────────────────────────────────────────────── */

template <class T, class Func> struct SpawnTask final : SpawnTaskBase {
  using V = typename FixVoid<T>::Type;

  Func                m_func;
  Own<PromiseNode<T>> m_node;
  std::atomic<bool>   m_resolved{false};

  explicit SpawnTask(Func f) : m_func(std::move(f)) {}

  bool poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    if (!m_node) {
      auto promise = m_func();
      m_node       = promise.release_node();
    }
    if (m_node->poll(waker)) {
      m_resolved.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  void wake_join() override {
    uint8_t expected = JoinEmpty;
    if (m_join_state.compare_exchange_strong(expected, JoinWoken,
                                             std::memory_order_acq_rel)) {
      return;
    }
    Waker jw     = m_join_waker;
    m_join_waker = Waker();
    jw.wake();
  }

  bool join_poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    m_join_waker = waker;
    uint8_t expected = JoinEmpty;
    if (!m_join_state.compare_exchange_strong(expected, JoinStored,
                                              std::memory_order_acq_rel)) {
      return true;
    }
    return false;
  }

  void *take_raw() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_acquire), "SpawnTask::take before completion");
    auto *val = new V(m_node->take());
    m_node    = nullptr;
    return val;
  }
};

template <class Func> struct SpawnTask<void, Func> final : SpawnTaskBase {
  Func                   m_func;
  Own<PromiseNode<void>> m_node;
  std::atomic<bool>      m_resolved{false};

  explicit SpawnTask(Func f) : m_func(std::move(f)) {}

  bool poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    if (!m_node) {
      auto promise = m_func();
      m_node       = promise.release_node();
    }
    if (m_node->poll(waker)) {
      m_node->take();
      m_resolved.store(true, std::memory_order_release);
      m_node = nullptr;
      return true;
    }
    return false;
  }

  void wake_join() override {
    uint8_t expected = JoinEmpty;
    if (m_join_state.compare_exchange_strong(expected, JoinWoken,
                                             std::memory_order_acq_rel)) {
      return;
    }
    Waker jw     = m_join_waker;
    m_join_waker = Waker();
    jw.wake();
  }

  bool join_poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    m_join_waker = waker;
    uint8_t expected = JoinEmpty;
    if (!m_join_state.compare_exchange_strong(expected, JoinStored,
                                              std::memory_order_acq_rel)) {
      return true;
    }
    return false;
  }

  void *take_raw() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_acquire), "SpawnTask::take before completion");
    if (m_node) {
      m_node->take();
      m_node = nullptr;
    }
    return nullptr;
  }
};

/* ── Worker ──────────────────────────────────────────────────────── */

class Worker;

/* ── WorkerSchedule ──────────────────────────────────────────────── */

struct WorkerSchedule : Schedule {
  Worker *worker;

  explicit WorkerSchedule(Worker *w) : worker(w) {}

  void schedule(SpawnTaskBase *task) override;
  void yield_now(SpawnTaskBase *task) override;
};

/* ── GlobalSchedule ──────────────────────────────────────────────── */

class GlobalSchedule : public Schedule {
public:
  explicit GlobalSchedule(Runtime *rt) : m_rt(rt) {}

  void schedule(SpawnTaskBase *task) override;

private:
  Runtime *m_rt;
};

/* ── Worker ──────────────────────────────────────────────────────── */

class Worker {
public:
  enum State : uint8_t {
    Quit,
    Idle,
    Running
  };

  Worker(size_t id, Runtime *rt, Box<void, EventLoopDeleter> loop)
      : m_id(id), m_rt(rt), m_loop(std::move(loop)), m_sched(this) {}

  size_t id() const noexcept {
    return m_id;
  }
  Runtime *rt() const noexcept {
    return m_rt;
  }
  Box<void, EventLoopDeleter> &loop() noexcept {
    return m_loop;
  }
  WorkStealingQueue<SpawnTaskBase *> &local_queue() noexcept {
    return m_local_queue;
  }
  WorkerSchedule &sched() noexcept {
    return m_sched;
  }

  State state(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return static_cast<State>(m_state.load(order));
  }
  void set_state(State s, std::memory_order order = std::memory_order_relaxed) noexcept {
    m_state.store(static_cast<uint8_t>(s), order);
  }

  bool alive(std::memory_order order = std::memory_order_acquire) const noexcept {
    return state(order) != Quit;
  }
  void shutdown(std::memory_order order = std::memory_order_release) noexcept {
    set_state(Quit, order);
  }

  uint32_t tick() const noexcept {
    return m_tick;
  }
  void inc_tick() noexcept {
    ++m_tick;
  }

private:
  size_t                             m_id;
  Runtime                           *m_rt;
  Box<void, EventLoopDeleter>        m_loop;
  WorkStealingQueue<SpawnTaskBase *> m_local_queue;
  WorkerSchedule                     m_sched;
  std::atomic<uint8_t>               m_state{Idle};
  uint32_t                           m_tick{0};
};

/* ── Thread-local Context ─────────────────────────────────────────── */

struct Context {
  Runtime   *runtime = nullptr;
  Worker    *worker  = nullptr;
  xEventLoop loop    = nullptr;
};

Context &current_context();

} // namespace _

/* ── EnterGuard ──────────────────────────────────────────────────── */

/**
 * @brief RAII guard that pushes a runtime context onto the current
 *        thread and pops it on destruction.
 *
 * Sets the thread-local Context (runtime, worker, event loop) on
 * construction and clears it on destruction.
 *
 * Usage in Runtime::block_on:
 *   EnterGuard guard(this, nullptr, m_main_loop.get());
 *   promise.wait(guard);
 * Usage in worker_main:
 *   EnterGuard guard(rt, w, w->loop().get());
 */
class EnterGuard {
public:
  EnterGuard(Runtime *rt, _::Worker *w, xEventLoop loop);
  ~EnterGuard();

  EnterGuard(const EnterGuard &)            = delete;
  EnterGuard &operator=(const EnterGuard &) = delete;

  xEventLoop loop() const { return m_loop; }

private:
  xEventLoop m_loop;
};

/* ── Runtime ─────────────────────────────────────────────────────── */

/* ── Runtime ─────────────────────────────────────────────────────── */

class Runtime {
public:
  explicit Runtime(size_t nthreads = 0);
  ~Runtime();

  Runtime(const Runtime &)            = delete;
  Runtime &operator=(const Runtime &) = delete;

  template <class Func>
  JoinHandle<typename ReducePromise<decltype(std::declval<Func>()())>::Type> spawn(Func &&func);

  /**
   * @brief Block until a callable's result promise completes.
   *
   * Calls enter() first, then invokes func() to create the promise,
   * then drives the event loop until resolved. Use this form when the
   * callable creates a coroutine that needs xpp::spawn() (the runtime
   * context must be active before the coroutine body starts).
   */
  template <class Func>
  auto block_on(Func &&func) -> decltype(func().wait(std::declval<EnterGuard &>()));

  /**
   * @brief Block until an existing promise completes.
   *
   * Note: if the promise was created by a coroutine that uses
   * xpp::spawn(), prefer the Func overload to ensure the runtime
   * context is active before coroutine creation.
   */
  template <class T> T block_on(Promise<T> promise);

  /** @brief Get the Runtime active on this thread (or nullptr). */
  static Runtime *current() {
    return _::current_context().runtime;
  }

private:
  friend class _::Worker;
  friend struct _::SpawnTaskBase;
  friend struct _::WorkerSchedule;
  friend class _::GlobalSchedule;

  /** @brief Try to steal a task from any other worker. */
  _::SpawnTaskBase *try_steal(size_t thief_id);

  /** @brief Batch-grab up to N tasks from the global queue into a worker's local queue. */
  size_t grab_global(_::Worker &w, size_t max_grab);

  /** @brief Push a task to the global queue (fallback when local is full). */
  void push_global(_::SpawnTaskBase *task);

  /** @brief Wake an idle worker. */
  void wake_idle_worker();

  /** @brief Worker main loop (runs as xTaskGroup task). */
  static void *worker_main(void *arg);

  size_t                                     m_num_workers;
  Vec<Box<_::Worker>>                        m_workers;
  Box<void, EventLoopDeleter>                m_main_loop;
  sys::Mutex<std::deque<_::SpawnTaskBase *>> m_global_queue;
  _::GlobalSchedule                          m_global_sched;
  Box<void, TaskGroupDeleter>                m_group; // last: destroyed first → joins workers
};

/* ── JoinHandle<T> ───────────────────────────────────────────────── */

/**
 * @brief Ownership token for a spawned task's result.
 *
 * Must be co_await'd, wait()'d, or detach()'d. Dropping without
 * any of these is a programming error (debug panic, release detach).
 */
template <class T> class JoinHandle {
public:
  JoinHandle() : m_task(nullptr) {}

  JoinHandle(JoinHandle &&o) noexcept : m_task(o.m_task) {
    o.m_task = nullptr;
  }
  JoinHandle &operator=(JoinHandle &&o) noexcept {
    if (this != &o) {
      release();
      m_task   = o.m_task;
      o.m_task = nullptr;
    }
    return *this;
  }
  JoinHandle(const JoinHandle &)            = delete;
  JoinHandle &operator=(const JoinHandle &) = delete;
  ~JoinHandle() {
    release();
  }

  void detach();

#if XPP_HAS_COROUTINES
  auto operator co_await() {
    struct Awaiter {
      _::SpawnTaskBase *&task_ref;

      bool await_ready() const {
        return false;
      }

      bool await_suspend(std::coroutine_handle<> h) {
        _::Schedule *sched = new _::CoroWakeSchedule(h, xpp::current_event_loop());
        return !task_ref->join_poll(_::Waker(sched, nullptr));
      }

      auto await_resume() -> typename _::PromiseNode<T>::ValueType {
        void *raw = task_ref->take_raw();
        delete task_ref;
        task_ref = nullptr;
        typename _::PromiseNode<T>::ValueType val;
        if (raw) {
          auto *box = static_cast<typename FixVoid<T>::Type *>(raw);
          val       = std::move(*box);
          delete box;
        }
        return val;
      }
    };
    XPP_ASSERT(m_task != nullptr, "co_await on empty JoinHandle");
    return Awaiter{m_task};
  }
#endif

private:
  friend class Runtime;

  explicit JoinHandle(_::SpawnTaskBase *task) : m_task(task) {}

  void release();

  _::SpawnTaskBase *m_task;
};

/* ── Runtime template implementations ────────────────────────────── */

template <class Func>
JoinHandle<typename ReducePromise<decltype(std::declval<Func>()())>::Type>
Runtime::spawn(Func &&func) {
  using T    = typename ReducePromise<decltype(std::declval<Func>()())>::Type;
  auto *task = new _::SpawnTask<T, typename std::decay<Func>::type>(std::forward<Func>(func));

  _::Worker *w = _::current_context().worker;
  if (w) {
    task->m_sched = &w->sched();
    if (w->local_queue().push(task)) {
      // fast path: pushed to local queue
    } else {
      push_global(task);
      wake_idle_worker();
    }
  } else {
    task->m_sched = &m_global_sched;
    push_global(task);
    wake_idle_worker();
  }

  return JoinHandle<T>{task};
}

template <class T> void JoinHandle<T>::detach() {
  if (!m_task) return;
  auto prev = m_task->state.exchange(_::SpawnTaskBase::Detached, std::memory_order_acq_rel);
  if (prev == _::SpawnTaskBase::Completed) {
    delete m_task;
  }
  m_task = nullptr;
}

template <class T> void JoinHandle<T>::release() {
  if (m_task) {
    XPP_DEBUG_ASSERT(false, "JoinHandle dropped without await or detach");
    detach();
  }
}

/* ── Promise<T>::wait ────────────────────────────────────────────── */

template <class T> T Promise<T>::wait(EnterGuard &guard) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");

  bool                fired = false;
  _::SyncWaitSchedule sync_sched(&fired, guard.loop());
  while (!m_node->poll(_::Waker(&sync_sched, nullptr))) {
    while (!fired)
      xEventWait(guard.loop(), -1);
    fired = false;
  }

  return m_node->take();
}

template <> inline void Promise<void>::wait(EnterGuard &guard) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");

  bool                fired = false;
  _::SyncWaitSchedule sync_sched(&fired, guard.loop());
  while (!m_node->poll(_::Waker(&sync_sched, nullptr))) {
    while (!fired)
      xEventWait(guard.loop(), -1);
    fired = false;
  }

  m_node->take();
}

/* ── Runtime::block_on ───────────────────────────────────────────── */

template <class Func>
auto Runtime::block_on(Func &&func) -> decltype(func().wait(std::declval<EnterGuard &>())) {
  EnterGuard guard(this, nullptr, m_main_loop.get());
  auto       promise = func();
  return promise.wait(guard);
}

template <class T> T Runtime::block_on(Promise<T> promise) {
  EnterGuard guard(this, nullptr, m_main_loop.get());
  return promise.wait(guard);
}

template <> inline void Runtime::block_on(Promise<void> promise) {
  EnterGuard guard(this, nullptr, m_main_loop.get());
  promise.wait(guard);
}

xEventLoop current_event_loop();

template <class Func>
JoinHandle<typename ReducePromise<decltype(std::declval<Func>()())>::Type> spawn(Func &&func) {
  auto *rt = Runtime::current();
  XPP_ASSERT(rt != nullptr, "xpp::spawn: no active runtime on this thread");
  return rt->spawn(std::forward<Func>(func));
}

} // namespace xpp

#endif // XPP_RUNTIME_H
