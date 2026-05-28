/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_node.h - Internal PromiseNode<T> hierarchy for xpp::Promise<T>.
 *
 * This header defines the virtual PromiseNode interface and the concrete
 * node types that implement Promise chaining, flattening, and deferred
 * resolution. Users should not include this directly — use <xpp/promise.h>.
 *
 * C++11-compatible. Header-only (templates).
 */

#ifndef XPP_PROMISE_NODE_H
#define XPP_PROMISE_NODE_H

#include <xpp/compiler.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/void.h>

#include <atomic>
#include <cstddef>
#include <utility>

#if XPP_HAS_COROUTINES
#include <coroutine>
#endif

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

class Runtime;

template <class T> class Promise;

/* ── ReturnType helper ───────────────────────────────────────────── */

template <class Func, class Arg>
using ReturnType = decltype(std::declval<Func>()(std::declval<Arg>()));

template <class Func> using ReturnTypeVoid = decltype(std::declval<Func>()());

/* ── ReducePromise ───────────────────────────────────────────────── */

template <class T> struct ReducePromise {
  using Type = T;
};
template <class U> struct ReducePromise<Promise<U>> {
  using Type = U;
};

namespace _ {

class Worker;

struct SpawnTaskBase;

/* ── Schedule ───────────────────────────────────────────────────── */

struct Schedule {
  virtual ~Schedule()                        = default;
  virtual void schedule(SpawnTaskBase *task) = 0;
  virtual void yield_now(SpawnTaskBase *task) {
    schedule(task);
  }
};

/* ── Waker ───────────────────────────────────────────────────────── */

class Waker {
public:
  Waker() : m_sched(nullptr), m_task(nullptr) {}
  Waker(Schedule *sched, SpawnTaskBase *task) : m_sched(sched), m_task(task) {}

  void wake() const {
    if (m_sched) m_sched->schedule(m_task);
  }

  Schedule *sched() const {
    return m_sched;
  }
  SpawnTaskBase *task() const {
    return m_task;
  }

private:
  Schedule      *m_sched;
  SpawnTaskBase *m_task;
};

/* ── SpawnTaskBase ──────────────────────────────────────────────── */

/**
 * @brief Type-erased base for spawned tasks.
 *
 * Only poll() and take_raw() are virtual (type-dependent). The join
 * protocol (join_poll / wake_join) is type-independent and implemented
 * directly here — no virtual dispatch needed.
 */
struct SpawnTaskBase {
  enum State : uint8_t {
    Pending,
    Running,
    Completed,
    Detached
  };

  enum JoinState : uint8_t {
    JoinEmpty,    // join_poll hasn't stored a waker yet
    JoinStored,  // join_poll stored a waker
    JoinWoken    // wake_join fired before join_poll stored
  };

  virtual ~SpawnTaskBase()       = default;
  virtual bool poll(Waker waker) = 0;
  virtual void *take_raw()       = 0;

  /**
   * @brief Called by JoinPromiseNode::poll to check/register for completion.
   *
   * Returns true if task already resolved (fast path). Otherwise stores
   * the waker and returns false — wake_join() will fire it later.
   */
  bool join_poll(Waker waker) {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    m_join_waker = waker;
    uint8_t expected = JoinEmpty;
    if (!m_join_state.compare_exchange_strong(expected, JoinStored,
                                              std::memory_order_acq_rel)) {
      // wake_join() already transitioned to JoinWoken — task is done.
      return true;
    }
    return false;
  }

  /**
   * @brief Called by worker_main after task completes to wake the joiner.
   *
   * If join_poll hasn't stored a waker yet (JoinEmpty), transitions to
   * JoinWoken so join_poll sees it immediately. Otherwise reads the
   * stored waker and fires it.
   */
  void wake_join() {
    uint8_t expected = JoinEmpty;
    if (m_join_state.compare_exchange_strong(expected, JoinWoken,
                                             std::memory_order_acq_rel)) {
      return;
    }
    // expected == JoinStored: waker is ready to fire.
    Waker jw     = m_join_waker;
    m_join_waker = Waker();
    jw.wake();
  }

  Schedule            *m_sched{nullptr};
  std::atomic<uint8_t> state{Pending};
  std::atomic<uint8_t> m_join_state{JoinEmpty};
  std::atomic<bool>    m_resolved{false};
  Waker                m_join_waker;
};

/* ── Schedule implementations for sync wait and coroutine wake ─── */

struct SyncWaitSchedule : Schedule {
  bool      *flag;
  xEventLoop loop;

  SyncWaitSchedule(bool *f, xEventLoop l) : flag(f), loop(l) {}

  void schedule(SpawnTaskBase *) override {
    if (!loop) {
      *flag = true;
      return;
    }
    xEventLoopPost(loop, [](void *arg) { *static_cast<bool *>(arg) = true; }, flag);
  }
};

#if XPP_HAS_COROUTINES
struct CoroWakeSchedule : Schedule {
  std::coroutine_handle<> handle;
  xEventLoop              loop;

  CoroWakeSchedule(std::coroutine_handle<> h, xEventLoop l) : handle(h), loop(l) {}

  void schedule(SpawnTaskBase *) override {
    if (!loop) {
      handle.resume();
      delete this;
      return;
    }
    xEventLoopPost(
      loop,
      [](void *arg) {
        auto *self = static_cast<CoroWakeSchedule *>(arg);
        self->handle.resume();
        delete self;
      },
      this);
  }
};
#endif

/* ── PromiseNode<T> ──────────────────────────────────────────────── */

/**
 * @brief Core async computation interface, analogous to Rust's Future trait.
 *
 * ## Comparison with rust::Future
 *
 *   Rust:  fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Output>
 *   xpp:   virtual bool poll(Waker waker)          →  true = Ready, false = Pending
 *          virtual ValueType take()                 →  extract the Output
 *
 * Similarities:
 *   - Both are polled by an executor/reactor; never call poll() yourself.
 *   - Both register a Waker during poll() — the executor will re-poll after
 *     wake() is called.
 *   - Both are "one-shot": once poll() returns true, it will not be called
 *     again; the value is consumed via take().
 *
 * Differences (driven by C++ constraints):
 *   - poll() returns bool instead of Poll<T>.  C++ cannot return
 *     "T or Pending" without variant overhead, so readiness and value
 *     extraction are split into two methods: poll() + take().
 *   - Waker is passed by value (cheap: one pointer) and copied freely,
 *     whereas Rust passes &mut Context which owns the Waker reference.
 *     This avoids lifetime complexity in C++11.
 *   - take() is separate from poll().  In Rust the Output is inside
 *     Poll::Ready(value); here you call take() only after poll() returned
 *     true.  This is safe because the executor guarantees no re-poll after
 *     readiness.
 *   - Template parameter T stays as the user-facing type (e.g. void, int),
 *     while ValueType = FixVoid<T>::Type maps void → Void internally so
 *     that take() always returns a movable value.
 *
 * ## How it works internally
 *
 * The event loop drives progress like this:
 *
 *   1. Executor calls node->poll(waker).
 *      - If the node is ready, poll() returns true.
 *      - Otherwise poll() stores the Waker and returns false.
 *   2. When some external event happens (fd readable, timer expired, …),
 *      the code that owns the stored Waker calls waker.wake().
 *   3. wake() posts a callback to the event loop via xEventLoopPost.
 *   4. On the next iteration the executor re-polls the node.
 *   5. When poll() finally returns true, the executor calls take() to
 *      consume the result.  After this the node is dead — never poll again.
 *
 * ## Implementing your own async logic
 *
 * Subclass PromiseNode<T> and implement poll() + take().
 *
 * Example — a node that resolves after a one-shot xEventTimer fires:
 *
 *   class TimerNode final : public xpp::_::PromiseNode<void> {
 *   public:
 *     TimerNode(xEventLoop loop, uint64_t delay_ms)
 *         : m_loop(loop), m_armed(false), m_fired(false) {
 *       m_timer = xEventLoopTimerAfter(loop, on_timer, this, delay_ms);
 *     }
 *     ~TimerNode() { xEventLoopTimerCancel(m_loop, m_timer); }
 *
 *     bool poll(xpp::_::Waker waker) override {
 *       if (m_fired) return true;
 *       m_waker = waker;
 *       return false;
 *     }
 *
 *     xpp::Void take() override { return xpp::Void{}; }
 *
 *   private:
 *     static void on_timer(void *arg) {
 *       auto *self = static_cast<TimerNode *>(arg);
 *       self->m_fired = true;
 *       self->m_waker.wake();
 *     }
 *
 *     xEventLoop   m_loop;
 *     xEventTimer  m_timer;
 *     bool         m_armed;
 *     bool         m_fired;
 *     xpp::_::Waker m_waker;
 *   };
 *
 * Wrap it into a Promise<void>:
 *
 *   xpp::Promise<void> delay(xEventLoop loop, uint64_t ms) {
 *     return xpp::Promise<void>(
 *       xpp::Own<xpp::_::PromiseNode<void>>(new TimerNode(loop, ms)));
 *   }
 *
 * Then use it with .then(), co_await, or .wait():
 *
 *   delay(loop, 100).then([] { printf("100ms passed\n"); });
 *
 * Key rules for implementations:
 *   - poll() must be idempotent: calling it again after returning true is UB.
 *   - Store the Waker when returning false; call wake() exactly once when
 *     the node becomes ready.
 *   - take() is called exactly once, after poll() returns true.
 *   - Do not call wake() inside poll() — the executor handles scheduling.
 */

template <class T> class PromiseNode {
public:
  using ValueType = typename FixVoid<T>::Type;

  virtual ~PromiseNode() = default;

  virtual bool      poll(Waker waker) = 0;
  virtual ValueType take()            = 0;
};

template <> class PromiseNode<void> {
public:
  using ValueType = Void;

  virtual ~PromiseNode() = default;

  virtual bool poll(Waker waker) = 0;
  virtual Void take()            = 0;
};

/* ── ImmediatePromiseNode ────────────────────────────────────── */

template <class T> class ImmediatePromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit ImmediatePromiseNode(T &&value) : m_val(std::move(value)) {}

  bool poll(Waker) override {
    return true;
  }
  ValueType take() override {
    return std::move(m_val);
  }

private:
  ValueType m_val;
};

template <> class ImmediatePromiseNode<void> final : public PromiseNode<void> {
public:
  bool poll(Waker) override {
    return true;
  }
  Void take() override {
    return Void{};
  }
};

/* ── TransformPromiseNode ────────────────────────────────────── */

template <class U, class T, class Func> class TransformPromiseNode final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(Own<PromiseNode<T>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  bool poll(Waker waker) override {
    return m_dep->poll(waker);
  }
  OutputType take() override {
    return ::xpp::_voidwrap::call1<U>(m_fn, m_dep->take());
  }

private:
  Own<PromiseNode<T>> m_dep;
  Func                m_fn;
};

template <class U, class Func>
class TransformPromiseNode<U, void, Func> final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(Own<PromiseNode<void>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  bool poll(Waker waker) override {
    return m_dep->poll(waker);
  }
  OutputType take() override {
    m_dep->take();
    return ::xpp::_voidwrap::call<U>(m_fn);
  }

private:
  Own<PromiseNode<void>> m_dep;
  Func                   m_fn;
};

template <class T, class Func>
class TransformPromiseNode<void, T, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(Own<PromiseNode<T>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  bool poll(Waker waker) override {
    return m_dep->poll(waker);
  }
  Void take() override {
    ::xpp::_voidwrap::call1<void>(m_fn, m_dep->take());
    return Void{};
  }

private:
  Own<PromiseNode<T>> m_dep;
  Func                m_fn;
};

template <class Func>
class TransformPromiseNode<void, void, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(Own<PromiseNode<void>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  bool poll(Waker waker) override {
    return m_dep->poll(waker);
  }
  Void take() override {
    m_dep->take();
    ::xpp::_voidwrap::call<void>(m_fn);
    return Void{};
  }

private:
  Own<PromiseNode<void>> m_dep;
  Func                   m_fn;
};

/* ── ChainPromiseNode ────────────────────────────────────────────── */

template <class T> class ChainPromiseNode final : public PromiseNode<T> {
public:
  explicit ChainPromiseNode(Own<PromiseNode<Promise<T>>> outer);

  bool                               poll(Waker waker) override;
  typename PromiseNode<T>::ValueType take() override;

private:
  enum State {
    Step1,
    Step2
  };
  State                        m_state;
  Own<PromiseNode<Promise<T>>> m_outer;
  Own<PromiseNode<T>>          m_inner;
};

/* ── AdapterPromiseNode ──────────────────────────────────────── */

template <class T> class AdapterPromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  AdapterPromiseNode() {}

  bool poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    m_waker = waker;
    // Double-check after storing waker: if resolve() raced between
    // the first check and the waker store, wake immediately.
    if (m_resolved.load(std::memory_order_acquire)) {
      m_waker.wake();
      return true;
    }
    return false;
  }

  ValueType take() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_relaxed),
               "AdapterPromiseNode::take before resolve");
    return std::move(m_val);
  }

  void resolve(T &&value) {
    XPP_ASSERT(!m_resolved.load(std::memory_order_relaxed),
               "AdapterPromiseNode resolved twice");
    m_val = std::move(value);
    // Release store: guarantees m_val is visible to any thread that
    // observes m_resolved == true via acquire load in poll().
    m_resolved.store(true, std::memory_order_release);
    m_waker.wake();
  }

private:
  ValueType          m_val;
  Waker              m_waker;
  std::atomic<bool>  m_resolved{false};
};

template <> class AdapterPromiseNode<Void> final : public PromiseNode<void> {
public:
  AdapterPromiseNode() {}

  bool poll(Waker waker) override {
    if (m_resolved.load(std::memory_order_acquire)) return true;
    m_waker = waker;
    if (m_resolved.load(std::memory_order_acquire)) {
      m_waker.wake();
      return true;
    }
    return false;
  }

  Void take() override {
    XPP_ASSERT(m_resolved.load(std::memory_order_relaxed),
               "AdapterPromiseNode::take before resolve");
    return Void{};
  }

  void resolve() {
    XPP_ASSERT(!m_resolved.load(std::memory_order_relaxed),
               "AdapterPromiseNode resolved twice");
    m_resolved.store(true, std::memory_order_release);
    m_waker.wake();
  }

private:
  Waker             m_waker;
  std::atomic<bool> m_resolved{false};
};

/* ── YieldPromiseNode ────────────────────────────────────────────── */

class YieldPromiseNode final : public PromiseNode<void> {
public:
  bool poll(Waker) override {
    return true;
  }
  Void take() override {
    return Void{};
  }
};

} // namespace _

} // namespace xpp

#endif // XPP_PROMISE_NODE_H
