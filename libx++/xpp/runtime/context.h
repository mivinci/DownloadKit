/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * context.h - Thread-local runtime context.
 *
 * Mirrors Tokio's runtime::context module. A single thread_local
 * Context tracks three independently-set fields:
 *
 *   - scheduler:  the SchedulerHandle active on this thread, used by
 *                 the free spawn() / spawn_blocking() to find a backend.
 *   - worker:     the running Worker, if any — enables the fast path
 *                 where spawn() pushes onto the worker's local queue.
 *   - loop:       the event loop that doubles as this thread's I/O
 *                 driver and park (libx xEventLoop). Read by the
 *                 contract-layer free function current_event_loop().
 *
 * Fields are installed by EnterGuard (in block_on) and by worker_main,
 * and restored on scope exit via SetContextGuard (Tokio's
 * SetCurrentGuard).
 */

#ifndef XPP_RUNTIME_CONTEXT_H
#define XPP_RUNTIME_CONTEXT_H

#include <utility>
#include <xpp/compiler.h>
#include <xpp/option.h>
#include <xpp/result.h>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

namespace runtime {

class Runtime;

namespace _ {
class SchedulerHandle; // level-2 flavor-dispatch handle (scheduler.h)
class Worker;          // multi-thread worker (multi_thread_scheduler.h)
} // namespace _

enum class TryCurrentError {
  NoContext,
};

/**
 * @brief Thread-local state for the current async execution context.
 */
class Context {
public:
  /** @brief The Context bound to the calling thread. */
  static Context &current();

  /** @brief Running worker, or nullptr when not on a worker thread. */
  _::Worker *worker() const noexcept {
    return m_worker;
  }

  /** @brief Event loop (driver + park) bound to this thread. */
  xEventLoop loop() const noexcept {
    return m_loop;
  }

  /** @brief The Runtime active on this thread, or nullptr. */
  Runtime *runtime() const noexcept {
    return m_runtime;
  }

  template <class Func>
  auto with_current(Func &&f)
    -> Result<decltype(f(std::declval<_::SchedulerHandle &>())), TryCurrentError> {
    return m_schedule_handle.map(std::move(f)).ok_or(TryCurrentError::NoContext);
  }

private:
  friend class SetContextGuard;

  Option<_::SchedulerHandle &> m_schedule_handle;
  _::Worker                   *m_worker{nullptr};
  xEventLoop                   m_loop{nullptr};
  Runtime                     *m_runtime{nullptr};
};

/**
 * @brief Installs context fields and restores the previous values on
 *        drop. Equivalent to Tokio's context::SetCurrentGuard.
 *
 * The fields are scoped: nested guards (e.g. a worker entering the
 * runtime that block_on already entered) save and restore correctly.
 */
class SetContextGuard {
public:
  SetContextGuard(_::SchedulerHandle *sched, _::Worker *worker, xEventLoop loop, Runtime *rt);
  ~SetContextGuard();

  /** @brief Movable: the source is disarmed so only one guard restores. */
  SetContextGuard(SetContextGuard &&o) noexcept
      : m_prev_sched(o.m_prev_sched), m_prev_worker(o.m_prev_worker), m_prev_loop(o.m_prev_loop),
        m_prev_runtime(o.m_prev_runtime), m_armed(o.m_armed) {
    o.m_armed = false;
  }

  SetContextGuard(const SetContextGuard &)            = delete;
  SetContextGuard &operator=(const SetContextGuard &) = delete;
  SetContextGuard &operator=(SetContextGuard &&)      = delete;

private:
  Option<_::SchedulerHandle &> m_prev_sched;
  _::Worker                   *m_prev_worker;
  xEventLoop                   m_prev_loop;
  Runtime                     *m_prev_runtime;
  bool                         m_armed{true};
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_CONTEXT_H
