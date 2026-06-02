/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime.cpp - Runtime lifecycle.
 */

#include <xpp/runtime/runtime.h>

#include <thread>

namespace xpp {
namespace runtime {

namespace {

size_t resolve_workers(size_t n) {
  if (n == 0) n = std::thread::hardware_concurrency();
  return n ? n : 4;
}

} // namespace

Runtime::Runtime(const Builder &builder)
    : m_driver(Driver::create()),
      m_blocking_pool(Arc<BlockingPool>::make(builder.m_max_blocking_threads)),
      m_scheduler(builder.m_flavor == RuntimeFlavor::CurrentThread
                    ? Scheduler::current_thread(m_blocking_pool)
                    : Scheduler::multi_thread(m_blocking_pool,
                                              resolve_workers(builder.m_worker_threads))),
      m_handle(m_scheduler.handle()), m_flavor(builder.m_flavor) {
  XPP_ASSERT(m_driver.handle() != nullptr, "Runtime: failed to create main event loop");
}

Runtime::~Runtime() = default;

} // namespace runtime
} // namespace xpp
