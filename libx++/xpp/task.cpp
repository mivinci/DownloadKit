/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.cpp - C++ RAII wrappers for xTaskGroup / xTask.
 */

#include <xpp/task.h>

namespace xpp {

// ── TaskGroup ──────────────────────────────────────────────────────────────

Result<TaskGroup, xErrno> TaskGroup::create(size_t nthreads) {
  xTaskGroupConf conf{};
  conf.nthreads = nthreads;
  xTaskGroup h  = xTaskGroupCreate(&conf);
  if (!h) return Result<TaskGroup, xErrno>(err, xErrno_NoMemory);
  return Result<TaskGroup, xErrno>(ok, TaskGroup(h, FromRaw{}));
}

Result<TaskGroup, xErrno> TaskGroup::create(const xTaskGroupConf &conf) {
  xTaskGroup h = xTaskGroupCreate(&conf);
  if (!h) return Result<TaskGroup, xErrno>(err, xErrno_NoMemory);
  return Result<TaskGroup, xErrno>(ok, TaskGroup(h, FromRaw{}));
}

Task TaskGroup::submit(xTaskFunc fn, void *arg) {
  return Task(xTaskSubmit(m_h, fn, arg));
}

xErrno TaskGroup::waitAll() {
  return xTaskGroupWait(m_h);
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
xErrno Task::wait(void **result) {
  return xTaskWait(m_h, result);
}
xErrno Task::cancel() {
  return xTaskCancel(m_h);
}

} // namespace xpp
