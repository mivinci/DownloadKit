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
#include <xpp/runtime/builder.h>
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

// RuntimeFlavor + Builder live in <xpp/runtime/builder.h>; the event
// loop driver (Driver / EventLoopDeleter) lives in <xpp/runtime/driver.h>.

/* ── Runtime ─────────────────────────────────────────────────────── */

class Runtime {
public:
  /**
   * @brief Build a multi-thread runtime (shortcut for
   *        Builder::new_multi_thread().worker_threads(nthreads)).
   * @param nthreads  Worker thread count; 0 = hardware concurrency.
   */
  explicit Runtime(size_t nthreads = 0)
      : Runtime(Builder::new_multi_thread().worker_threads(nthreads)) {}

  /**
   * @brief Build a runtime of the given flavor (shortcut over Builder).
   * @param nthreads  Worker count for MultiThread (0 = hardware
   *                  concurrency); ignored for CurrentThread.
   */
  explicit Runtime(RuntimeFlavor flavor, size_t nthreads = 0)
      : Runtime(flavor == RuntimeFlavor::CurrentThread
                  ? Builder::new_current_thread()
                  : Builder::new_multi_thread().worker_threads(nthreads)) {}

  /** @brief Construct from a fully-configured Builder. */
  explicit Runtime(const Builder &builder);

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

  /** @brief This runtime's flavor (CurrentThread or MultiThread). */
  RuntimeFlavor flavor() const noexcept {
    return m_flavor;
  }

  /** @brief The main thread's event loop (driver + park) handle. */
  xEventLoop main_loop() const noexcept {
    return m_driver.handle();
  }

private:
  Driver            m_driver;
  Arc<BlockingPool> m_blocking_pool;
  Scheduler         m_scheduler;
  Handle            m_handle;
  RuntimeFlavor     m_flavor;
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
