/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime.h - Async runtime composing scheduler + handle + blocking pool.
 *
 * Architecture (mirrors Tokio):
 *   Runtime { Scheduler, Handle, BlockingPool }
 *
 * The scheduler owns workers and work-stealing logic.  The blocking
 * pool runs off-event-loop callables.  Handle is the cloneable facade
 * for spawning.  block_on drives a promise to completion on the calling
 * thread's main event loop (the block_on driver/park).
 */

#ifndef XPP_RUNTIME_RUNTIME_H
#define XPP_RUNTIME_RUNTIME_H

#include <xpp/arc.h>
#include <xpp/box.h>
#include <xpp/promise.h>

#include <xpp/runtime/blocking_pool.h>
#include <xpp/runtime/context.h>
#include <xpp/runtime/driver.h>
#include <xpp/runtime/handle.h>
#include <xpp/runtime/scheduler.h>

#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {

// The event loop driver (Driver / EventLoopDeleter) lives in
// <xpp/runtime/driver.h>.

/* ── Runtime ─────────────────────────────────────────────────────── */

class Runtime {
public:
  /**
   * @brief Build a multi-thread, work-stealing runtime (heap-owned;
   *        Runtime is pinned, hence returned in a Box).
   * @param worker_threads       Worker count; 0 = hardware concurrency.
   * @param max_blocking_threads Thread cap for spawn_blocking work.
   */
  static Box<Runtime> new_multi_thread(size_t worker_threads = 0, size_t max_blocking_threads = 512);

  /**
   * @brief Build a single-threaded runtime (tasks run on the block_on
   *        thread; heap-owned).
   * @param max_blocking_threads Thread cap for spawn_blocking work.
   */
  static Box<Runtime> new_current_thread(size_t max_blocking_threads = 512);

  ~Runtime();

  Runtime(const Runtime &)            = delete;
  Runtime &operator=(const Runtime &) = delete;

  /** @brief Spawn an async task onto the runtime. */
  template <class Func>
  auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    return m_handle.spawn(std::forward<Func>(f));
  }

  /** @brief Run a blocking callable on the blocking pool. */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
    return m_handle.spawn_blocking(std::forward<F>(f));
  }

  /** @brief Drive an existing promise to completion (entry point). */
  template <class T> T block_on(Promise<T> promise);

  /**
   * @brief Enter the runtime context, then build and drive a promise.
   *
   * The context is active before func() runs, so a coroutine body that
   * calls xpp::spawn() sees the runtime. Prefer this form when func
   * creates a coroutine.
   */
  template <class Func>
  auto block_on(Func &&func) ->
    typename ReducePromise<decltype(std::declval<Func>()())>::Type;

  /** @brief Cloneable handle to this runtime. */
  const Handle &handle() const noexcept {
    return m_handle;
  }

  /**
   * @brief Enter the runtime context on the calling thread.
   *
   * Returns an RAII guard that, for its lifetime, makes xpp::spawn /
   * spawn_blocking and current_event_loop() target this runtime —
   * useful outside block_on (e.g. spawning from a test before awaiting).
   * To drive a future to completion, use block_on instead.
   */
  EnterGuard enter() {
    return EnterGuard(m_scheduler.handle(), m_driver.handle(), this);
  }

  /** @brief The Runtime active on the calling thread, or nullptr. */
  static Runtime *current() {
    return Context::current().runtime();
  }

  /** @brief The main thread's event loop (driver + park) handle. */
  xEventLoop main_loop() const noexcept {
    return m_driver.handle();
  }

private:
  // Internal flavor selector for the shared real constructor. Runtime
  // has no public constructors: callers build one via the static
  // new_multi_thread / new_current_thread factories.
  enum class Kind { CurrentThread, MultiThread };
  Runtime(Kind kind, size_t worker_threads, size_t max_blocking_threads);

  Driver            m_driver;
  Arc<BlockingPool> m_blocking_pool;
  Scheduler         m_scheduler;
  Handle            m_handle;
};

/* ── block_on implementations ────────────────────────────────────── */

template <class T> T Runtime::block_on(Promise<T> promise) {
  EnterGuard             guard(m_scheduler.handle(), m_driver.handle(), this);
  Own<_::PromiseNode<T>> node = promise.release_node();
  m_scheduler.block_on_drive(m_driver, node.get());
  return node->take();
}

template <> inline void Runtime::block_on(Promise<void> promise) {
  EnterGuard                guard(m_scheduler.handle(), m_driver.handle(), this);
  Own<_::PromiseNode<void>> node = promise.release_node();
  m_scheduler.block_on_drive(m_driver, node.get());
  node->take();
}

template <class Func>
auto Runtime::block_on(Func &&func) ->
  typename ReducePromise<decltype(std::declval<Func>()())>::Type {
  using U = typename ReducePromise<decltype(std::declval<Func>()())>::Type;
  // Enter first so a coroutine body may call xpp::spawn() before its
  // first suspend; block_on(Promise) re-enters (harmless, nested).
  EnterGuard guard(m_scheduler.handle(), m_driver.handle(), this);
  return block_on<U>(func());
}

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_RUNTIME_H
