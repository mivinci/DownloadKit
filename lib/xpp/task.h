/*
 * Copyright 2025 The libxpp Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.h - C++ RAII wrappers for xTaskGroup / xTask.
 */

#ifndef XPP_TASK_H
#define XPP_TASK_H

#include "handle.h"
#include "result.h"

#include <xbase/task.h>

#include <cstddef>

namespace xpp {

class Task;

/**
 * @brief RAII wrapper around xTaskGroup (thread pool).
 *
 * Move-only. Destructor calls xTaskGroupDestroy which waits for all
 * pending tasks to complete, then releases the handle.
 */
class TaskGroup : public Handle<xTaskGroup, xTaskGroupDestroy> {
  using Base = Handle<xTaskGroup, xTaskGroupDestroy>;

public:
  /**
   * @brief Create a task group with the given number of worker threads.
   *
   * @param nthreads  Number of worker threads. 0 = auto-detect.
   * @return          Ok(TaskGroup) on success, Err(xErrno) on failure.
   */
  static Result<TaskGroup, xErrno> create(size_t nthreads = 0);

  /**
   * @brief Create a task group from a full configuration struct.
   *
   * @param conf  Configuration (nthreads, queue_cap, etc.).
   * @return      Ok(TaskGroup) on success, Err(xErrno) on failure.
   */
  static Result<TaskGroup, xErrno> create(const xTaskGroupConf &conf);

  using Base::Base;
  using Base::operator=;

  /**
   * @brief Submit a task to the group for execution.
   *
   * @param fn   The function to execute on a worker thread.
   * @param arg  Argument passed to @p fn.
   * @return     A Task handle, or an empty Task on failure.
   */
  Task submit(xTaskFunc fn, void *arg);

  /**
   * @brief Wait for all pending tasks in the group to complete.
   * @return xErrno_Ok on success.
   */
  xErrno waitAll();

  /**
   * @brief Get the number of worker threads in the group.
   * @return Number of worker threads.
   */
  size_t threads() const;

  /**
   * @brief Get the number of pending tasks (queued + running).
   * @return Number of pending tasks.
   */
  size_t pending() const;

private:
  TaskGroup(xTaskGroup h, FromRaw) noexcept : Base(h, FromRaw{}) {}
};

/**
 * @brief RAII wrapper around xTask (submitted work item).
 *
 * Non-owning: destruction does NOT wait or cancel — the parent
 * TaskGroup's destructor handles that. Move-only.
 */
class Task {
public:
  Task() noexcept = default;
  ~Task()         = default;

  Task(Task &&o) noexcept;
  Task &operator=(Task &&o) noexcept;
  Task(const Task &)            = delete;
  Task &operator=(const Task &) = delete;

  /**
   * @brief Access the underlying C handle.
   * @return The raw xTask handle, or nullptr if empty.
   */
  xTask handle() const noexcept;

  /**
   * @brief Check whether the task handle is non-null.
   * @return True if the handle is valid.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Wait for the task to complete.
   *
   * @param result  If non-NULL, receives the return value of the task function.
   * @return        xErrno_Ok on success, xErrno_Cancelled if the task was cancelled.
   */
  xErrno wait(void **result = nullptr);

  /**
   * @brief Attempt to cancel a queued task.
   *
   * If the task is still waiting in the queue, it is marked as cancelled
   * and will not be executed. The caller may safely release the task's
   * argument after a successful cancel.
   *
   * If the task is already running or has completed, the cancel fails
   * and xErrno_Busy is returned. In that case the caller must call
   * wait() before releasing the argument.
   *
   * @return xErrno_Ok if cancelled, xErrno_Busy if already running or finished.
   */
  xErrno cancel();

private:
  explicit Task(xTask h) noexcept;
  xTask m_h = nullptr;
  friend class TaskGroup;
};

} // namespace xpp

#endif // XPP_TASK_H
