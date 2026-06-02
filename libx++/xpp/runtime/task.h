/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.h - Spawned-task types: SpawnTask<T, Func> and JoinPromiseNode<T>.
 *
 * These are the runtime's task module (analogous to Tokio's
 * runtime/task/). They depend only on the contract layer
 * (<xpp/promise.h>) so that schedulers and the blocking pool can
 * include them without pulling in the scheduler/handle headers.
 *
 * The free spawn() / spawn_blocking() API lives in <xpp/runtime/spawn.h>.
 */

#ifndef XPP_RUNTIME_TASK_H
#define XPP_RUNTIME_TASK_H

#include <xpp/panic.h>
#include <xpp/promise.h>

#include <atomic>
#include <utility>

namespace xpp {
namespace runtime {
namespace _ {

// Bring the contract-internal types (PromiseNode, Waker, SpawnTaskBase,
// AdapterPromiseNode, ...) into scope so runtime-internal code can refer
// to them with a bare `_::` prefix from xpp::runtime scope.
using namespace ::xpp::_;

/* ── SpawnTask<T, Func> ──────────────────────────────────────────── */

/**
 * @brief Concrete spawned task holding the factory closure + promise node.
 *
 * Unified for all T (including void) — FixVoid maps void → Void at the
 * PromiseNode level, so the code is identical regardless of T. The
 * factory closure m_func() is invoked lazily on first poll() (on the
 * worker thread), yielding the Promise<T> whose node is then driven.
 */
template <class T, class Func> struct SpawnTask final : SpawnTaskBase {
  Func                m_func;
  Own<PromiseNode<T>> m_node;

  explicit SpawnTask(Func f) : m_func(std::move(f)) {}

  bool poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    if (!m_node) {
      auto promise = m_func();
      m_node       = promise.release_node();
    }
    if (m_node->poll(waker)) {
      m_resolved.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  void *take_raw() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_acquire), "SpawnTask::take before completion");
    return m_node.get();
  }
};

/* ── JoinPromiseNode<T> ──────────────────────────────────────────── */

/**
 * @brief PromiseNode that bridges a spawned task's join protocol.
 *
 * This is the JoinHandle role: spawn() returns Promise<T> backed by
 * this node. Drop semantics: if the Promise (and thus this node) is
 * destroyed before take(), the underlying task is detached — it runs
 * to completion and then self-deletes.
 */
template <class T> class JoinPromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit JoinPromiseNode(SpawnTaskBase *task) : m_task(task) {}

  ~JoinPromiseNode() {
    if (!m_task) return;
    // Detach: task runs to completion and self-deletes.
    auto prev = m_task->state.exchange(SpawnTaskBase::Detached, std::memory_order_acq_rel);
    if (prev == SpawnTaskBase::Completed) delete m_task;
  }

  bool poll(Waker waker) override {
    return m_task->join_poll(waker);
  }

  ValueType take() override {
    auto     *node = static_cast<PromiseNode<T> *>(m_task->take_raw());
    ValueType val  = node->take();
    delete m_task;
    m_task = nullptr;
    return val;
  }

private:
  SpawnTaskBase *m_task;
};

template <> inline Void JoinPromiseNode<void>::take() {
  auto *node = static_cast<PromiseNode<void> *>(m_task->take_raw());
  node->take();
  delete m_task;
  m_task = nullptr;
  return Void{};
}

} // namespace _
} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_TASK_H
