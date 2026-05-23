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
 *    ├── Worker[0..N]: lazily spawned, capped at CPU core count
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
 *     5. execute task: promise.wait(scope) drives the worker's loop
 *     6. post result back to caller's loop via xEventLoopPost
 *
 *   Worker scaling:
 *     - Workers start at 0, created on demand by wake_idle_worker()
 *     - Cap = hardware concurrency (or user-specified)
 *     - Workers never shrink (parked idle workers are cheap)
 *
 * Design inspired by Tokio (Rust) and Go runtime.
 */

#ifndef XPP_RUNTIME_H
#define XPP_RUNTIME_H

#include <xpp/compiler.h>
#include <xpp/mutex.h>
#include <xpp/promise.h>

#include <atomic>
#include <deque>

extern "C" {
#include <x/base/event.h>
#include <x/base/task.h>
}

#if XPP_HAS_COROUTINES
#include <coroutine>
#endif

namespace xpp {

class Runtime;
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

/* ── SpawnTaskBase (type-erased) ─────────────────────────────────── */

struct Worker; // forward declaration

struct SpawnTaskBase {
  virtual ~SpawnTaskBase()               = default;
  virtual void execute(WaitScope &scope) = 0;
  Worker      *target_worker; // set by spawn(), used by post callback
};

/* ── Helpers: void vs non-void dispatch ──────────────────────────── */

template <class T> typename FixVoid<T>::Type wait_and_get(Promise<T> &p, WaitScope &scope) {
  return p.wait(scope);
}

inline Void wait_and_get(Promise<void> &p, WaitScope &scope) {
  p.wait(scope);
  return Void{};
}

template <class V> void resolve_adapter(AdapterPromiseNode<V> *a, V &&val) {
  a->resolve(std::move(val));
}

inline void resolve_adapter(AdapterPromiseNode<Void> *a, Void &&) {
  a->resolve();
}

/* ── SpawnTask<T> ────────────────────────────────────────────────── */

/**
 * @brief Typed spawned task state.
 *
 * Lifecycle:
 *   1. Created by Runtime::spawn() on the caller thread.
 *   2. Pushed to a worker's local queue.
 *   3. Worker pops it, calls execute() → drives promise to completion.
 *   4. Posts resolve_on_caller back to caller's event loop.
 *   5. Caller's adapter resolves → JoinHandle's co_await/wait unblocks.
 *   6. Deleted by JoinHandle (await/wait) or self (if detached).
 *
 * Thread safety:
 *   - execute() runs exclusively on one worker thread.
 *   - resolve_on_caller() runs on the caller thread (via post).
 *   - No concurrent access to any field.
 */
template <class T> struct SpawnTask final : SpawnTaskBase {
  using V = typename FixVoid<T>::Type;

  Promise<T>             promise;
  AdapterPromiseNode<V> *adapter;
  xEventLoop             caller_loop;
  bool                   detached;
  bool                   completed;
  V                      result{};

  SpawnTask(Promise<T> p, AdapterPromiseNode<V> *a, xEventLoop loop)
      : promise(std::move(p)), adapter(a), caller_loop(loop), detached(false), completed(false) {}

  void execute(WaitScope &scope) override {
    result = wait_and_get(promise, scope);
    xEventLoopPost(caller_loop, resolve_on_caller, this);
  }

  static void resolve_on_caller(void *arg) {
    auto *self      = static_cast<SpawnTask *>(arg);
    self->completed = true;
    resolve_adapter(self->adapter, std::move(self->result));
    if (self->detached) {
      delete self;
    }
  }
};

/* ── Worker ──────────────────────────────────────────────────────── */

struct Worker {
  enum State : uint8_t {
    Idle,
    Running
  };

  size_t                             id;
  Runtime                           *rt;
  xEventLoop                         loop;
  WorkStealingQueue<SpawnTaskBase *> local_queue;
  std::atomic<bool>                  running;
  std::atomic<uint8_t>               state; // Idle or Running
};

/** @brief Thread-local: current worker on this thread (nullptr if not a worker). */
static thread_local Worker *tl_current_worker = nullptr;

} // namespace _

/* ── Runtime ─────────────────────────────────────────────────────── */

class Runtime {
public:
  explicit Runtime(size_t nthreads = 0);
  ~Runtime();

  Runtime(const Runtime &)            = delete;
  Runtime &operator=(const Runtime &) = delete;

  template <class T> JoinHandle<T> spawn(Promise<T> promise);

  template <class T> T block_on(Promise<T> promise);

private:
  friend struct _::Worker;

  /** @brief Try to steal a task from any other worker. */
  _::SpawnTaskBase *try_steal(size_t thief_id);

  /** @brief Batch-grab up to N tasks from the global queue into a worker's local queue. */
  size_t grab_global(_::Worker &w, size_t max_grab);

  /** @brief Push a task to the global queue (fallback when local is full). */
  void push_global(_::SpawnTaskBase *task);

  /** @brief Wake an idle worker, or spawn a new one if under the cap. */
  void wake_idle_worker();

  /** @brief Spawn a new worker thread (lazy initialization). */
  void spawn_worker();

  /** @brief Worker main loop (runs as xTaskGroup task). */
  static void *worker_main(void *arg);

  size_t                                m_max_workers;     // cap (CPU core count)
  std::atomic<size_t>                   m_active_workers;  // currently alive
  _::Worker                            *m_workers;         // pre-allocated array[m_max_workers]
  xTaskGroup                            m_group;
  xEventLoop                            m_main_loop;
  Mutex<std::deque<_::SpawnTaskBase *>> m_global_queue;
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

  JoinHandle(JoinHandle &&o) noexcept : m_task(o.m_task), m_promise(std::move(o.m_promise)) {
    o.m_task = nullptr;
  }
  JoinHandle &operator=(JoinHandle &&o) noexcept {
    if (this != &o) {
      release();
      m_task    = o.m_task;
      m_promise = std::move(o.m_promise);
      o.m_task  = nullptr;
    }
    return *this;
  }
  JoinHandle(const JoinHandle &)            = delete;
  JoinHandle &operator=(const JoinHandle &) = delete;
  ~JoinHandle() {
    release();
  }

  void detach();
  T    wait(WaitScope &scope);

#if XPP_HAS_COROUTINES
  bool await_ready() const {
    return false;
  }
  bool await_suspend(std::coroutine_handle<> h) {
    XPP_ASSERT(m_task != nullptr, "co_await on empty JoinHandle");
    return m_promise.await_suspend(h);
  }
  T await_resume();
#endif

private:
  friend class Runtime;

  JoinHandle(_::SpawnTask<T> *task, Promise<T> promise)
      : m_task(task), m_promise(std::move(promise)) {}

  void release();

  _::SpawnTask<T> *m_task;
  Promise<T>       m_promise;
};

/* ── Runtime template implementations ────────────────────────────── */

template <class T> JoinHandle<T> Runtime::spawn(Promise<T> promise) {
  using V = typename FixVoid<T>::Type;

  xEventLoop caller_loop = WaitScope::current_loop();
  if (!caller_loop) caller_loop = m_main_loop;

  auto               *adapter = new _::AdapterPromiseNode<V>();
  Own<_::PromiseNode> node{adapter};
  Promise<T>          join_promise{std::move(node)};

  auto *task = new _::SpawnTask<T>(std::move(promise), adapter, caller_loop);

  // Scheduling strategy (Go-style):
  //   1. If caller is on a worker thread, try pushing to its local queue.
  //   2. If local queue is full (or caller is not a worker), push to global queue.
  // Workers will: pop local → batch grab global → steal other workers.

  _::Worker *w = _::tl_current_worker;
  if (w && w->local_queue.push(task)) {
    // Fast path: pushed to current worker's local queue. No post needed
    // because the worker will see it on its next pop().
  } else {
    // Slow path: push to global queue. Wake an idle worker to pick it up.
    push_global(task);
    wake_idle_worker();
  }

  return JoinHandle<T>{task, std::move(join_promise)};
}

template <class T> T Runtime::block_on(Promise<T> promise) {
  WaitScope scope(m_main_loop);
  return promise.wait(scope);
}

template <> inline void Runtime::block_on<void>(Promise<void> promise) {
  WaitScope scope(m_main_loop);
  promise.wait(scope);
}

/* ── JoinHandle template implementations ─────────────────────────── */

template <class T> void JoinHandle<T>::detach() {
  if (!m_task) return;
  m_task->detached = true;
  if (m_task->completed) delete m_task;
  m_task    = nullptr;
  m_promise = Promise<T>();
}

template <class T> T JoinHandle<T>::wait(WaitScope &scope) {
  XPP_ASSERT(m_task != nullptr, "wait on empty JoinHandle");
  T result = m_promise.wait(scope);
  delete m_task;
  m_task = nullptr;
  return result;
}

template <> inline void JoinHandle<void>::wait(WaitScope &scope) {
  XPP_ASSERT(m_task != nullptr, "wait on empty JoinHandle");
  m_promise.wait(scope);
  delete m_task;
  m_task = nullptr;
}

#if XPP_HAS_COROUTINES
template <class T> T JoinHandle<T>::await_resume() {
  XPP_ASSERT(m_task != nullptr, "await_resume on empty JoinHandle");
  T result = m_promise.await_resume();
  delete m_task;
  m_task = nullptr;
  return result;
}

template <> inline void JoinHandle<void>::await_resume() {
  XPP_ASSERT(m_task != nullptr, "await_resume on empty JoinHandle");
  m_promise.await_resume();
  delete m_task;
  m_task = nullptr;
}
#endif

template <class T> void JoinHandle<T>::release() {
  if (m_task) {
    XPP_DEBUG_ASSERT(false, "JoinHandle dropped without await or detach");
    detach();
  }
}

} // namespace xpp

#endif // XPP_RUNTIME_H
