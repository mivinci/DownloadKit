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
 *     6. on completion, wake the join waker (resumes co_await)
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

/* ── SpawnTask ───────────────────────────────────────────────────── */

/**
 * @brief Concrete spawned task holding the factory closure + promise node.
 *
 * Unified for all T (including void) — FixVoid maps void → Void at
 * the PromiseNode level, so the code is identical regardless of T.
 */
template <class T, class Func> struct SpawnTask final : SpawnTaskBase {
  Func                m_func;
  Own<PromiseNode<T>> m_node;

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

  void *take_raw() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_acquire), "SpawnTask::take before completion");
    return m_node.get();
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

/**
 * @brief Per-worker thread state.
 *
 * ## State vs Shutdown — separation of concerns (Tokio pattern)
 *
 * `m_state` (Idle/Running) is the worker's *activity* signal, used by
 * `wake_idle_worker()` to find a parked worker. It is written ONLY by
 * the worker thread itself — no external writer.
 *
 * `m_shutdown` is a write-once flag set by the Runtime destructor. The
 * worker reads it each iteration to decide whether to exit.
 *
 * Keeping these on separate atomics eliminates the race where a worker's
 * `set_state(Idle)` could overwrite a destructor's shutdown store — the
 * bug that previously caused ~50% hang rate on destruction.
 */
class Worker {
public:
  enum State : uint8_t {
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

  /** Activity state — written only by the worker thread. */
  State state(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return static_cast<State>(m_state.load(order));
  }
  void set_state(State s, std::memory_order order = std::memory_order_relaxed) noexcept {
    m_state.store(static_cast<uint8_t>(s), order);
  }

  /** Shutdown flag — write-once by destructor, read by worker. */
  bool alive(std::memory_order order = std::memory_order_acquire) const noexcept {
    return !m_shutdown.load(order);
  }
  void shutdown(std::memory_order order = std::memory_order_release) noexcept {
    m_shutdown.store(true, order);
  }

  /** Exit acknowledgment — set by worker after leaving the main loop. */
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
  Runtime                           *m_rt;
  Box<void, EventLoopDeleter>        m_loop;
  WorkStealingQueue<SpawnTaskBase *> m_local_queue;
  WorkerSchedule                     m_sched;
  std::atomic<uint8_t>               m_state{Idle};     // activity (worker-owned)
  std::atomic<bool>                  m_shutdown{false}; // stop signal (destructor-owned)
  std::atomic<bool>                  m_exited{false};   // ack (worker-owned)
  uint32_t                           m_tick{0};
};

/* ── Thread-local Context ─────────────────────────────────────────── */

struct Context {
  Runtime   *runtime = nullptr;
  Worker    *worker  = nullptr;
  xEventLoop loop    = nullptr;
};

Context &current_context();

/* ── JoinPromiseNode<T> ──────────────────────────────────────────── */

/**
 * @brief PromiseNode that bridges a spawned task's join protocol.
 *
 * This replaces JoinHandle — spawn() now returns Promise<T> directly.
 * Drop semantics: if the Promise (and thus this node) is destroyed
 * before take(), the underlying task is detached (runs to completion,
 * then self-deletes).
 */
template <class T> class JoinPromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit JoinPromiseNode(SpawnTaskBase *task) : m_task(task) {}

  ~JoinPromiseNode() {
    if (!m_task) return;
    // Detach: task runs to completion and self-deletes.
    auto prev = m_task->state.exchange(SpawnTaskBase::Detached, std::memory_order_acq_rel);
    if (prev == SpawnTaskBase::Completed) delete m_task;
  }

  bool poll(Waker waker) override {
    return m_task->join_poll(waker);
  }

  ValueType take() override {
    auto     *node = static_cast<PromiseNode<T> *>(m_task->take_raw());
    ValueType val  = node->take();
    delete m_task;
    m_task = nullptr;
    return val;
  }

private:
  SpawnTaskBase *m_task;
};

template <> inline Void JoinPromiseNode<void>::take() {
  auto *node = static_cast<PromiseNode<void> *>(m_task->take_raw());
  node->take();
  delete m_task;
  m_task = nullptr;
  return Void{};
}

/**
 * @brief Spawn task that adapts an external (blocking) resolution into
 *        a Promise<R>.
 *
 * One heap object holds everything: the AdapterPromiseNode the joiner
 * polls (resolved on the m_group thread), and the user callable F.
 * Never scheduled on a worker — `poll()` exists only to satisfy
 * SpawnTaskBase's pure virtual.
 *
 * Lifetime: deleted by JoinPromiseNode::take() on the success path, or
 * by submit_blocking's done_fn on the detached path.
 */
template <class R, class F> struct BlockingSpawnTask final : SpawnTaskBase {
  AdapterPromiseNode<R> node; // adapter the Promise<R> waits on
  F                     func; // user callable, run by run_on_worker

  explicit BlockingSpawnTask(F f) : func(std::move(f)) {}

  /// Called on the m_group blocking thread.
  void run_on_worker() {
    node.resolve(func());
  }

  bool poll(Waker) override {
    return false;
  } // never invoked
  void *take_raw() override {
    return &node;
  }
};

template <class F> struct BlockingSpawnTask<void, F> final : SpawnTaskBase {
  AdapterPromiseNode<Void> node;
  F                        func;

  explicit BlockingSpawnTask(F f) : func(std::move(f)) {}

  void run_on_worker() {
    func();
    node.resolve();
  }

  bool poll(Waker) override {
    return false;
  }
  void *take_raw() override {
    return &node;
  }
};

} // namespace _

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

  EnterGuard(EnterGuard &&o) noexcept : m_loop(o.m_loop) {
    o.m_loop = nullptr;
  }
  EnterGuard &operator=(EnterGuard &&) = delete;

  EnterGuard(const EnterGuard &)            = delete;
  EnterGuard &operator=(const EnterGuard &) = delete;

  xEventLoop loop() const {
    return m_loop;
  }

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
  Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> spawn(Func &&func);

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

  /**
   * @brief Enter the runtime context on this thread.
   *
   * Returns an RAII guard that sets the thread-local event loop
   * (and optional Runtime pointer) for the guard's lifetime.
   * This is the preferred way to make xpp async primitives work
   * outside of block_on — e.g. in tests.
   */
  EnterGuard enter() {
    return EnterGuard(this, nullptr, m_main_loop.get());
  }

  /** @brief Get the Runtime active on this thread (or nullptr). */
  static Runtime *current() {
    return _::current_context().runtime;
  }

  /**
   * @brief Run a blocking function on the runtime's xTaskGroup thread.
   *
   * The callable is executed synchronously on an m_group worker thread.
   * Its return value is delivered as a Promise<R> resolved on the calling
   * async worker's event loop — safe to co_await or pass to spawn().
   *
   * Mirrors Tokio's tokio::task::spawn_blocking.
   */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())>;

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

  /**
   * @brief Blocking template helper: submit run_on_worker() to m_group.
   *
   * work_fn runs task->run_on_worker() on an m_group thread (resolves
   * task->node).  done_fn runs on the calling worker's event loop:
   * sets m_resolved on the SpawnTask and wakes the join waker so
   * JoinPromiseNode::poll unblocks.
   *
   * @param task  Newly-created BlockingSpawnTask<R, F>* that owns both
   *              the adapter node and the blocking callable.
   */
  template <class R, class F> void submit_blocking(_::BlockingSpawnTask<R, F> *task);

  size_t                                     m_num_workers;
  Vec<Box<_::Worker>>                        m_workers;
  Box<void, EventLoopDeleter>                m_main_loop;
  sys::Mutex<std::deque<_::SpawnTaskBase *>> m_global_queue;
  _::GlobalSchedule                          m_global_sched;
  Box<void, TaskGroupDeleter>                m_group; // last: destroyed first → joins workers
};

/* ── Runtime template implementations ────────────────────────────── */

template <class Func>
Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type>
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

  return Promise<T>(Own<_::PromiseNode<T>>(new _::JoinPromiseNode<T>(task)));
}

/* ── Runtime::spawn_blocking ──────────────────────────────────────── */

template <class R, class F> void Runtime::submit_blocking(_::BlockingSpawnTask<R, F> *task) {
  xEventLoop loop = _::current_context().loop;

  xErrno rc = xEventLoopSubmit(
    loop, m_group.get(),
    /* work_fn: run blocking func on an m_group worker thread */
    [](void *arg) -> void * {
      auto *t = static_cast<_::BlockingSpawnTask<R, F> *>(arg);
      t->run_on_worker(); // blocks until func() returns; resolves t->node
      return nullptr;
    },
    /* done_fn: on loop thread — m_resolved and wake_join so joiners unblock */
    [](void *arg, void * /*result*/) {
      auto *t = static_cast<_::BlockingSpawnTask<R, F> *>(arg);
      t->m_resolved.store(true, std::memory_order_release);
      t->wake_join();
      // If the joiner already detached the task, it's our job to delete it.
      uint8_t prev = t->state.exchange(_::SpawnTaskBase::Completed, std::memory_order_acq_rel);
      if (prev == _::SpawnTaskBase::Detached) delete t;
    },
    task, nullptr /* xEventWork* out */
  );

  if (rc != xErrno_Ok) {
    // Submission failed — surface to the joiner instead of hanging.
    // task->node was never resolved; mark resolved-with-no-value so
    // join_poll returns true and the eventual take() asserts cleanly
    // (better than a silent hang).  Caller should treat this as fatal.
    task->m_resolved.store(true, std::memory_order_release);
    task->wake_join();
    XPP_ASSERT(false, "xEventLoopSubmit failed in spawn_blocking");
  }
}

template <class F> auto Runtime::spawn_blocking(F &&f) -> Promise<decltype(f())> {
  using R  = decltype(f());
  using FD = typename std::decay<F>::type;

  auto *task = new _::BlockingSpawnTask<R, FD>(std::forward<F>(f));

  submit_blocking(task);

  return Promise<R>(Own<_::PromiseNode<R>>(new _::JoinPromiseNode<R>(task)));
}

/* ── Promise<T>::wait ────────────────────────────────────────────── */

namespace _ {

template <class T> XPP_NOINLINE void poll_until_ready(PromiseNode<T> *node, xEventLoop loop) {
  bool             fired = false;
  SyncWaitSchedule sync_sched(&fired, loop);
  while (!node->poll(Waker(&sync_sched, nullptr))) {
    while (!fired)
      xEventWait(loop, -1);
    fired = false;
  }
}

} // namespace _

template <class T> T Promise<T>::wait(EnterGuard &guard) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");
  _::poll_until_ready(m_node.get(), guard.loop());
  return m_node->take();
}

template <> inline void Promise<void>::wait(EnterGuard &guard) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");
  _::poll_until_ready(m_node.get(), guard.loop());
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
Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> spawn(Func &&func) {
  auto *rt = Runtime::current();
  XPP_ASSERT(rt != nullptr, "xpp::spawn: no active runtime on this thread");
  return rt->spawn(std::forward<Func>(func));
}

/**
 * @brief Run a blocking function on the runtime's task group thread.
 *
 * The function executes on an m_group worker thread (off the event loop).
 * Its return value is delivered as a Promise<R> resolved on the calling
 * async worker's event loop — safe to co_await or pass to spawn().
 *
 * Mirrors Tokio's tokio::task::spawn_blocking.
 */
template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
  auto *rt = Runtime::current();
  XPP_ASSERT(rt != nullptr, "xpp::spawn_blocking: no active runtime on this thread");
  return rt->spawn_blocking(std::forward<F>(f));
}

} // namespace xpp

#endif // XPP_RUNTIME_H
