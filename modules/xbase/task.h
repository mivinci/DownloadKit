/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.h - N:M concurrent task model
 *
 * Provides a lightweight task abstraction where N tasks are multiplexed
 * onto M OS threads managed by a task group (thread pool).
 */

#ifndef XBASE_TASK_H
#define XBASE_TASK_H

#include <stddef.h>
#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Task function signature.
 * @param arg User-provided argument.
 * @return A user-defined result pointer, retrievable via xTaskWait.
 */
typedef void *(*xTaskFunc)(void *arg);

/**
 * @brief Opaque handle to a submitted task.
 */
XDEF_HANDLE(xTask);

/**
 * @brief Configuration for creating a task group.
 * @ingroup xTask
 */
XDEF_STRUCT(xTaskGroupConf) {
  size_t nthreads;  /**< Number of worker threads (M). 0 = auto-detect. */
  size_t queue_cap; /**< Task queue capacity. 0 = unbounded. */
};

/**
 * @brief Opaque handle to a task group (thread pool).
 */
XDEF_HANDLE(xTaskGroup);

/**
 * @brief Create a task group with the given configuration.
 * @ingroup xTask
 * @param conf Configuration. NULL for defaults.
 * @return A new task group, or NULL on failure.
 */
XCAPI(xTaskGroup) xTaskGroupCreate(const xTaskGroupConf *conf);

/**
 * @brief Destroy a task group.
 *
 * Waits for all pending tasks to complete, then releases resources.
 *
 * @ingroup xTask
 * @param g The task group to destroy.
 */
XCAPI(void) xTaskGroupDestroy(xTaskGroup g);

/**
 * @brief Submit a task to the group for execution.
 * @ingroup xTask
 * @param g The task group.
 * @param fn The function to execute.
 * @param arg The argument passed to fn.
 * @return A task handle, or NULL on failure.
 */
XCAPI(xTask) xTaskSubmit(xTaskGroup g, xTaskFunc fn, void *arg);

/**
 * @brief Wait for a specific task to complete.
 * @ingroup xTask
 * @param t The task handle.
 * @param result If non-NULL, receives the return value of the task function.
 * @return xErrno_Ok on success, xErrno_Cancelled if the task was cancelled.
 */
XCAPI(xErrno) xTaskWait(xTask t, void **result);

/**
 * @brief Attempt to cancel a queued task.
 *
 * If the task is still waiting in the queue, it is marked as cancelled
 * and will not be executed.  The caller may safely release the task's
 * argument after a successful cancel.
 *
 * If the task is already running or has completed, the cancel fails
 * and xErrno_Busy is returned.  In that case the caller must call
 * xTaskWait() before releasing the argument.
 *
 * @ingroup xTask
 * @param t The task handle.
 * @return xErrno_Ok if cancelled successfully, xErrno_InvalidState if the task
 *         is already running or finished.
 */
XCAPI(xErrno) xTaskCancel(xTask t);

/**
 * @brief Wait for all pending tasks in the group to complete.
 * @ingroup xTask
 * @param g The task group.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xTaskGroupWait(xTaskGroup g);

/**
 * @brief Get the number of worker threads in the group.
 * @ingroup xTask
 * @param g The task group.
 * @return Number of worker threads.
 */
XCAPI(size_t) xTaskGroupThreads(xTaskGroup g);

/**
 * @brief Get the number of pending tasks in the group.
 * @ingroup xTask
 * @param g The task group.
 * @return Number of tasks waiting or running.
 */
XCAPI(size_t) xTaskGroupPending(xTaskGroup g);

/**
 * @brief Get the global shared task group.
 *
 * Returns a lazily-initialized shared task group with default
 * configuration (unlimited threads, no queue cap). The returned
 * group is managed internally and must NOT be passed to
 * xTaskGroupDestroy().
 *
 * @ingroup xTask
 * @return The global task group, or NULL on initialization failure.
 */
XCAPI(xTaskGroup) xTaskGroupGlobal(void);

#endif // XBASE_TASK_H
