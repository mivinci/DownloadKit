/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * blocking_pool.cpp - Off-event-loop blocking work executor.
 */

#include <xpp/runtime/blocking_pool.h>

namespace xpp {
namespace runtime {

static void *create_pool_group(size_t capacity) {
  xTaskGroupConf conf = {capacity, 0};
  return xTaskGroupCreate(&conf);
}

BlockingPool::BlockingPool(size_t capacity)
    : m_group(Box<void, TaskGroupDeleter>::from_raw(create_pool_group(capacity))) {
  XPP_ASSERT(m_group.get() != nullptr, "BlockingPool: failed to create task group");
}

BlockingPool::~BlockingPool() = default;

} // namespace runtime
} // namespace xpp
