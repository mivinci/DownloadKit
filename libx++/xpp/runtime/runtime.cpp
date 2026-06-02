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

Runtime::Runtime(Kind kind, size_t worker_threads, size_t max_blocking_threads)
    : m_driver(Driver::create()),
      m_blocking_pool(Arc<BlockingPool>::make(max_blocking_threads)),
      m_scheduler(kind == Kind::CurrentThread
                    ? Scheduler::current_thread(m_blocking_pool)
                    : Scheduler::multi_thread(m_blocking_pool, resolve_workers(worker_threads))),
      m_handle(m_scheduler.handle()) {
  XPP_ASSERT(m_driver.handle() != nullptr, "Runtime: failed to create main event loop");
}

Box<Runtime> Runtime::new_multi_thread(size_t worker_threads, size_t max_blocking_threads) {
  return Box<Runtime>::from_raw(new Runtime(Kind::MultiThread, worker_threads, max_blocking_threads));
}

Box<Runtime> Runtime::new_current_thread(size_t max_blocking_threads) {
  return Box<Runtime>::from_raw(new Runtime(Kind::CurrentThread, 0, max_blocking_threads));
}

Runtime::~Runtime() = default;

} // namespace runtime
} // namespace xpp
