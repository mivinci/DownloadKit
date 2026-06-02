/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * context.cpp - Thread-local Context + SetContextGuard.
 *
 * Also defines the contract-layer free function current_event_loop()
 * (declared in <xpp/promise_node.h>), which the runtime owns because
 * it is the holder of the thread-local Context.
 */

#include <xpp/runtime/context.h>

#include <xpp/promise_node.h>

namespace xpp {
namespace runtime {

/* ── Thread-local Context ────────────────────────────────────────── */

static thread_local Context tl_context;

Context &Context::current() { return tl_context; }

/* ── SetContextGuard ─────────────────────────────────────────────── */

SetContextGuard::SetContextGuard(_::SchedulerHandle *sched, _::Worker *worker, xEventLoop loop,
                                 Runtime *rt) {
  Context &c       = tl_context;
  m_prev_sched     = c.m_schedule_handle;
  m_prev_scheduler = c.m_scheduler;
  m_prev_runtime   = c.m_runtime;
  c.m_schedule_handle =
    sched ? Option<_::SchedulerHandle &>(*sched) : Option<_::SchedulerHandle &>(none);
  c.m_scheduler.worker = worker;
  c.m_scheduler.loop   = loop;
  c.m_runtime          = rt;
}

SetContextGuard::~SetContextGuard() {
  if (!m_armed) return; // moved-from: the moved-to guard owns the restore
  Context &c          = tl_context;
  c.m_schedule_handle = m_prev_sched;
  c.m_scheduler       = m_prev_scheduler;
  c.m_runtime         = m_prev_runtime;
}

} // namespace runtime

/* ── Contract-layer hook ─────────────────────────────────────────── */

xEventLoop current_event_loop() { return runtime::Context::current().loop(); }

} // namespace xpp
