/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * blocking_pool.h - Off-event-loop blocking work executor.
 *
 * Runs callables on xTaskGroup threads (mirrors Tokio's
 * spawn_blocking).  Owns the underlying xTaskGroup.
 */

#ifndef XPP_RUNTIME_BLOCKING_POOL_H
#define XPP_RUNTIME_BLOCKING_POOL_H

#include <xpp/box.h>
#include <xpp/promise.h>
#include <xpp/runtime/task.h>

#include <type_traits>
#include <utility>

extern "C" {
#include <x/base/event.h>
#include <x/base/task.h>
}

namespace xpp {
namespace runtime {
namespace _ {

/* ── BlockingSpawnTask ───────────────────────────────────────────── */

/**
 * @brief Spawn task that adapts an external (blocking) resolution into
 *        a Promise<R>. Never scheduled on a worker — poll() exists only
 *        to satisfy SpawnTaskBase. Contract-internal types are visible
 *        via the using-directive in <xpp/runtime/task.h>.
 */
template <class R, class F> struct BlockingSpawnTask final : SpawnTaskBase {
  AdapterPromiseNode<R> node;
  F                     func;

  explicit BlockingSpawnTask(F f) : func(std::move(f)) {}

  void run_on_worker() {
    node.resolve(func());
  }

  bool poll(Waker) override {
    return false;
  }
  void *take_raw() override {
    return &node;
  }
};

template <class F> struct BlockingSpawnTask<void, F> final : SpawnTaskBase {
  AdapterPromiseNode<Void> node;
  F                        func;

  explicit BlockingSpawnTask(F f) : func(std::move(f)) {}

  void run_on_worker() {
    func();
    node.resolve();
  }

  bool poll(Waker) override {
    return false;
  }
  void *take_raw() override {
    return &node;
  }
};

} // namespace _

/* ── TaskGroupDeleter ────────────────────────────────────────────── */

struct TaskGroupDeleter {
  void operator()(void *p) const noexcept {
    xTaskGroupDestroy(p);
  }
};

/* ── BlockingPool ────────────────────────────────────────────────── */

class BlockingPool {
public:
  /**
   * @param capacity  Max concurrent threads for blocking work (mirrors
   *                  Tokio's max_blocking_threads). Worker threads run
   *                  in the scheduler's own group, not here.
   */
  explicit BlockingPool(size_t capacity);
  ~BlockingPool();

  BlockingPool(const BlockingPool &)            = delete;
  BlockingPool &operator=(const BlockingPool &) = delete;

  /**
   * @brief Run a blocking callable on the pool and return a Promise<R>.
   *
   * The callable executes off the event loop on an xTaskGroup worker.
   * Its return value is delivered as a Promise resolved on the calling
   * worker's event loop — safe to co_await.
   */
  template <class F> auto spawn(F &&f) -> Promise<decltype(f())>;

private:
  template <class R, class F> void submit_blocking(_::BlockingSpawnTask<R, F> *task);

  Box<void, TaskGroupDeleter> m_group;
};

/* ── Template implementations ────────────────────────────────────── */

template <class R, class F> void BlockingPool::submit_blocking(_::BlockingSpawnTask<R, F> *task) {
  xEventLoop loop = current_event_loop();

  xErrno rc = xEventLoopSubmit(
    loop, m_group.get(),
    /* work_fn */
    [](void *arg) -> void * {
      auto *t = static_cast<_::BlockingSpawnTask<R, F> *>(arg);
      t->run_on_worker();
      return nullptr;
    },
    /* done_fn */
    [](void *arg, void *) {
      auto *t = static_cast<_::BlockingSpawnTask<R, F> *>(arg);
      t->m_resolved.store(true, std::memory_order_release);
      t->wake_join();
      uint8_t prev = t->state.exchange(_::SpawnTaskBase::Completed, std::memory_order_acq_rel);
      if (prev == _::SpawnTaskBase::Detached) delete t;
    },
    task, nullptr);

  if (rc != xErrno_Ok) {
    task->m_resolved.store(true, std::memory_order_release);
    task->wake_join();
    XPP_ASSERT(false, "xEventLoopSubmit failed in spawn_blocking");
  }
}

template <class F> auto BlockingPool::spawn(F &&f) -> Promise<decltype(f())> {
  using R  = decltype(f());
  using FD = typename std::decay<F>::type;

  auto *task = new _::BlockingSpawnTask<R, FD>(std::forward<F>(f));
  submit_blocking(task);
  return Promise<R>(Own<_::PromiseNode<R>>(new _::JoinPromiseNode<R>(task)));
}

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_BLOCKING_POOL_H
