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
 */
typedef void (*xTaskFunc)(void *arg);

/**
 * @brief Opaque handle to a submitted task.
 */
XDEF_HANDLE(xTask);

/**
 * @brief Configuration for creating a task group.
 * @ingroup xTask
 */
XDEF_STRUCT(xTaskGroupConf) {
  size_t nthreads;    /**< Number of worker threads (M). 0 = auto-detect. */
  size_t queue_cap;   /**< Task queue capacity. 0 = unbounded. */
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
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xTaskWait(xTask t);

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

#endif // XBASE_TASK_H
