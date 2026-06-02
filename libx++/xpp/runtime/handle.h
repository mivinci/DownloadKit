/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * handle.h - Handle (public spawn facade) + EnterGuard.
 *
 * Handle is the level-1, cheaply-cloneable facade over the level-2
 * SchedulerHandle (mirrors Tokio's runtime::Handle { inner:
 * scheduler::Handle }). EnterGuard installs the scheduler handle (and
 * event loop) on the thread-local Context for its lifetime, so
 * xpp::spawn / current_event_loop work — mirroring Tokio's EnterGuard.
 */

#ifndef XPP_RUNTIME_HANDLE_H
#define XPP_RUNTIME_HANDLE_H

#include <xpp/box.h>
#include <xpp/promise.h>

#include <xpp/runtime/context.h>
#include <xpp/runtime/scheduler.h>

#include <utility>

namespace xpp {
namespace runtime {

/* ── EnterGuard ──────────────────────────────────────────────────── */

/**
 * @brief RAII guard that enters the runtime context on the current
 *        thread and exits it on drop.
 *
 * Owns its SchedulerHandle on the heap so that the address the
 * thread-local Context points at stays stable across moves — which lets
 * enter() return the guard by value (mirroring Tokio's EnterGuard). The
 * inner SetContextGuard transfers the restore duty on move, so exactly
 * one guard restores the previous context.
 */
class EnterGuard {
public:
  EnterGuard(const _::SchedulerHandle &sched, xEventLoop loop, Runtime *rt)
      : m_sched(Box<_::SchedulerHandle>::from_raw(new _::SchedulerHandle(sched))),
        m_guard(m_sched.get(), nullptr, loop, rt) {}

  EnterGuard(EnterGuard &&)            = default;
  EnterGuard &operator=(EnterGuard &&) = delete;
  EnterGuard(const EnterGuard &)            = delete;
  EnterGuard &operator=(const EnterGuard &) = delete;

private:
  // Heap-stable handle: declared before m_guard so it outlives the
  // Context pointer m_guard installs, and survives moves unmoved.
  Box<_::SchedulerHandle> m_sched;
  SetContextGuard         m_guard;
};

/* ── Handle ──────────────────────────────────────────────────────── */

/**
 * @brief Cheap-to-clone handle to a runtime. Spawn from anywhere.
 */
class Handle {
public:
  explicit Handle(_::SchedulerHandle inner) : m_inner(std::move(inner)) {}

  /** @brief Spawn an async task onto the runtime. */
  template <class Func>
  auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    return m_inner.spawn(std::forward<Func>(f));
  }

  /** @brief Run a blocking callable on the runtime's blocking pool. */
  template <class F> auto spawn_blocking(F &&f) -> Promise<decltype(f())> {
    return m_inner.spawn_blocking(std::forward<F>(f));
  }

  const _::SchedulerHandle &scheduler() const noexcept {
    return m_inner;
  }

private:
  _::SchedulerHandle m_inner;
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_HANDLE_H
