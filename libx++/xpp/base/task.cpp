/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.cpp - C++ RAII wrappers for xTaskGroup / xTask.
 */

#include <xpp/base/task.h>

namespace xpp {
namespace {

// Bridge a raw xErrno return into Result<void, Error>. xpp::Error is
// C-vocabulary-agnostic, so we cast through int here; the caller can
// still recover the original code via Error::code() and compare against
// xErrno_* constants. Mirrors the helper in event.cpp; kept private to
// each TU since the indirection is too small to warrant a shared header.
inline Result<void, Error> from_errno(xErrno rc) {
  if (rc == xErrno_Ok) return Result<void, Error>(ok);
  return Result<void, Error>(err, Error{static_cast<int>(rc)});
}

} // namespace

// ── TaskGroup ──────────────────────────────────────────────────────────────

Result<TaskGroup, Error> TaskGroup::create(size_t nthreads) {
  xTaskGroupConf conf{};
  conf.nthreads = nthreads;
  xTaskGroup h  = xTaskGroupCreate(&conf);
  if (!h) return Result<TaskGroup, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<TaskGroup, Error>(ok, TaskGroup(h, FromRaw{}));
}

Result<TaskGroup, Error> TaskGroup::create(const xTaskGroupConf &conf) {
  xTaskGroup h = xTaskGroupCreate(&conf);
  if (!h) return Result<TaskGroup, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<TaskGroup, Error>(ok, TaskGroup(h, FromRaw{}));
}

Task TaskGroup::submit(xTaskFunc fn, void *arg) {
  return Task(xTaskSubmit(m_h, fn, arg));
}

Result<void, Error> TaskGroup::waitAll() {
  return from_errno(xTaskGroupWait(m_h));
}
size_t TaskGroup::threads() const {
  return xTaskGroupThreads(m_h);
}
size_t TaskGroup::pending() const {
  return xTaskGroupPending(m_h);
}

// ── Task ───────────────────────────────────────────────────────────────────

Task::Task(xTask h) noexcept : m_h(h) {}

Task::Task(Task &&o) noexcept : m_h(o.m_h) {
  o.m_h = nullptr;
}

Task &Task::operator=(Task &&o) noexcept {
  if (this != &o) {
    m_h   = o.m_h;
    o.m_h = nullptr;
  }
  return *this;
}

xTask Task::handle() const noexcept {
  return m_h;
}
Task::operator bool() const noexcept {
  return m_h != nullptr;
}
Result<void, Error> Task::wait(void **result) {
  return from_errno(xTaskWait(m_h, result));
}
Result<void, Error> Task::cancel() {
  return from_errno(xTaskCancel(m_h));
}

} // namespace xpp
