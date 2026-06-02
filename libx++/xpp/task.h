/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.h - Public task API: spawn() / spawn_blocking().
 *
 * Mirrors Tokio's crate-root `tokio::task` module (tokio/src/task/),
 * which is distinct from the internal task machinery in
 * <xpp/runtime/task.h> (Tokio's tokio/src/runtime/task/).
 *
 * These route to the SchedulerHandle installed on the current thread's
 * Context, and panic when called outside a runtime context — mirroring
 * tokio::spawn ("must be called from the context of a Tokio runtime").
 */

#ifndef XPP_TASK_H
#define XPP_TASK_H

#include <xpp/panic.h>
#include <xpp/promise.h>

#include <xpp/runtime/context.h>
#include <xpp/runtime/scheduler.h>

#include <utility>

namespace xpp {

/* ── Free spawn / spawn_blocking functions ──────────────────────── */

template <class Func>
auto spawn(Func &&f) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
  using Ret = Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type>;
  return runtime::Context::current()
    .with_current([&](runtime::_::SchedulerHandle &h) { return h.spawn(std::forward<Func>(f)); })
    .unwrap_or_else([](runtime::TryCurrentError) -> Ret {
      XPP_PANIC("spawn() called outside of a runtime context");
    });
}

template <class Func> auto spawn_blocking(Func &&f) -> Promise<decltype(f())> {
  using Ret = Promise<decltype(f())>;
  return runtime::Context::current()
    .with_current(
      [&](runtime::_::SchedulerHandle &h) { return h.spawn_blocking(std::forward<Func>(f)); })
    .unwrap_or_else([](runtime::TryCurrentError) -> Ret {
      XPP_PANIC("spawn_blocking() called outside of a runtime context");
    });
}

} // namespace xpp

#endif // XPP_TASK_H
