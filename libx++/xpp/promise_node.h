/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_node.h - Internal PromiseNode hierarchy for xpp::Promise<T>.
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

#include <cstddef>
#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

template <class T> class Promise;

/* ── ReturnType helper ───────────────────────────────────────────── */

template <class Func, class Arg>
using ReturnType = decltype(std::declval<Func>()(std::declval<Arg>()));

/* Specialization for void argument (Func takes no args) */
template <class Func>
using ReturnTypeVoid = decltype(std::declval<Func>()());

/* ── ReducePromise ───────────────────────────────────────────────── */

template <class T> struct ReducePromise { using Type = T; };
template <class U> struct ReducePromise<Promise<U>> { using Type = U; };

namespace _ {

/* ── Event ───────────────────────────────────────────────────────── */

/**
 * @brief A one-shot event that can be armed (scheduled) on the current
 *        thread's event loop via xEventLoopPost.
 *
 * Subclass and override fire() for custom behavior. RootEvent is the
 * simple "set a flag" variant used by wait().
 */
class Event {
public:
  Event() : m_fired(false) {}
  virtual ~Event() = default;

  /**
   * @brief Schedule this event to fire on the current event loop.
   *
   * Uses xEventLoopPost to enqueue. Safe to call from the loop thread.
   */
  void arm();

  bool fired() const { return m_fired; }

  virtual void fire() { m_fired = true; }

protected:
  bool m_fired;
};

/**
 * @brief Simple event that just sets a flag when fired. Used by wait().
 */
class RootEvent final : public Event {
public:
  void fire() override { m_fired = true; }
};

/**
 * @brief Event used by PromiseNode::poll. Tracks whether poll has been
 *        called and whether the node is already ready.
 */
class PollEvent {
public:
  PollEvent() : m_ready(false) {}

  void init(Option<Event &> event) {
    if (m_ready) {
      if (event.is_some()) event.unwrap().arm();
    } else {
      m_event = event;
    }
  }

  void arm() {
    m_ready = true;
    if (m_event.is_some()) {
      m_event.unwrap().arm();
      m_event = none;
    }
  }

  bool is_ready() const { return m_ready; }

private:
  Option<Event &> m_event;
  bool m_ready;
};

/* ── PromiseNode ─────────────────────────────────────────────────── */

class PromiseNode;
/**
 * @brief Base class for all promise nodes.
 *
 * @code
 *   poll(event)  — "When you're ready, arm this event."
 *   read(dest)   — "Give me your result." (only valid after poll fires)
 * @endcode
 */
class PromiseNode {
public:
  virtual ~PromiseNode() = default;

  /**
   * @brief Register an event to be armed when this node is ready.
   *
   * If already ready, arm immediately.
   *
   * @param event  Event to arm when ready, or none to skip
   *               registration. Caller guarantees the referent outlives
   *               this node (typically stack-allocated in wait() or a
   *               ChainPromiseNode member).
   */
  virtual void poll(Option<Event &> event) = 0;

  /**
   * @brief Read the result into a type-erased destination.
   *
   * May only be called after poll() has armed the event. Moves the
   * result out — calling read() twice is undefined.
   *
   * @param dest  Pointer to storage of the correct type (caller casts).
   */
  virtual void read(void *dest) = 0;
};

/* ── ImmediatePromiseNode ────────────────────────────────────────── */

/**
 * @brief A node that already has its value. poll() fires immediately.
 */
template <class T>
class ImmediatePromiseNode final : public PromiseNode {
public:
  explicit ImmediatePromiseNode(T &&value) : m_value(std::move(value)) {}

  void poll(Option<Event &> event) override {
    if (event.is_some()) event.unwrap().arm();
  }

  void read(void *dest) override {
    *static_cast<T *>(dest) = std::move(m_value);
  }

private:
  T m_value;
};

/* ── TransformPromiseNode ────────────────────────────────────────── */

/**
 * @brief Applies a function to the result of a dependency node.
 *
 * Created by Promise<T>::then(). Polls the dependency; when ready,
 * read() pulls from dep, applies func, and writes the result.
 *
 * @tparam U     Output type (FixVoid<ReturnType<Func, T>>::Type)
 * @tparam T     Input type from dependency
 * @tparam Func  Callable T → U
 */
template <class U, class T, class Func>
class TransformPromiseNode final : public PromiseNode {
public:
  TransformPromiseNode(Own<PromiseNode> dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Option<Event &> event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    T dep_value;
    m_dep->read(&dep_value);
    *static_cast<U *>(dest) = m_func(std::move(dep_value));
  }

private:
  Own<PromiseNode> m_dep;
  Func m_func;
};

/* Specialization: T=Void (dependency is Promise<void>) */
template <class U, class Func>
class TransformPromiseNode<U, Void, Func> final : public PromiseNode {
public:
  TransformPromiseNode(Own<PromiseNode> dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Option<Event &> event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    Void v;
    m_dep->read(&v);
    *static_cast<U *>(dest) = m_func();
  }

private:
  Own<PromiseNode> m_dep;
  Func m_func;
};

/* Specialization: U=Void (func returns void) */
template <class T, class Func>
class TransformPromiseNode<Void, T, Func> final : public PromiseNode {
public:
  TransformPromiseNode(Own<PromiseNode> dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Option<Event &> event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    T dep_value;
    m_dep->read(&dep_value);
    m_func(std::move(dep_value));
    *static_cast<Void *>(dest) = Void{};
  }

private:
  Own<PromiseNode> m_dep;
  Func m_func;
};

/* Specialization: both Void → Void */
template <class Func>
class TransformPromiseNode<Void, Void, Func> final : public PromiseNode {
public:
  TransformPromiseNode(Own<PromiseNode> dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Option<Event &> event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    Void v;
    m_dep->read(&v);
    m_func();
    *static_cast<Void *>(dest) = Void{};
  }

private:
  Own<PromiseNode> m_dep;
  Func m_func;
};

/* ── ChainPromiseNode ────────────────────────────────────────────── */

/**
 * @brief Flattens Promise<Promise<T>> into Promise<T>.
 *
 * Two-step state machine:
 *   Step1: poll outer node → when ready, read out inner Promise, extract node
 *   Step2: poll inner node → when ready, read out final value
 */
class ChainPromiseNode final : public PromiseNode, public Event {
public:
  explicit ChainPromiseNode(Own<PromiseNode> inner)
      : m_state(Step1), m_inner(std::move(inner)) {
    m_inner->poll(Option<Event &>(*this));
  }

  void poll(Option<Event &> event) override {
    switch (m_state) {
    case Step1:
      m_outer_event = event;
      break;
    case Step2:
      m_inner->poll(event);
      break;
    }
  }

  void read(void *dest) override {
    XPP_ASSERT(m_state == Step2, "ChainPromiseNode::read before step2");
    m_inner->read(dest);
  }

protected:
  void fire() override;

private:
  enum State { Step1, Step2 };
  State m_state;
  Own<PromiseNode> m_inner;
  Option<Event &> m_outer_event;
};

/* ── AdapterPromiseNode ──────────────────────────────────────────── */

/**
 * @brief A node resolved externally via Resolver<T>.
 *
 * poll() registers the waiting event; resolve() stores the value and
 * arms the event.
 */
template <class T>
class AdapterPromiseNode final : public PromiseNode {
public:
  AdapterPromiseNode() : m_resolved(false) {}

  void poll(Option<Event &> event) override {
    if (m_resolved) {
      if (event.is_some()) event.unwrap().arm();
    } else {
      m_poll.init(event);
    }
  }

  void read(void *dest) override {
    XPP_ASSERT(m_resolved, "AdapterPromiseNode::read before resolve");
    *static_cast<T *>(dest) = std::move(m_value);
  }

  void resolve(T &&value) {
    XPP_ASSERT(!m_resolved, "AdapterPromiseNode resolved twice");
    m_value = std::move(value);
    m_resolved = true;
    m_poll.arm();
  }

private:
  T m_value;
  PollEvent m_poll;
  bool m_resolved;
};

/* Specialization for Void */
template <>
class AdapterPromiseNode<Void> final : public PromiseNode {
public:
  AdapterPromiseNode() : m_resolved(false) {}

  void poll(Option<Event &> event) override {
    if (m_resolved) {
      if (event.is_some()) event.unwrap().arm();
    } else {
      m_poll.init(event);
    }
  }

  void read(void *dest) override {
    XPP_ASSERT(m_resolved, "AdapterPromiseNode::read before resolve");
    *static_cast<Void *>(dest) = Void{};
  }

  void resolve() {
    XPP_ASSERT(!m_resolved, "AdapterPromiseNode resolved twice");
    m_resolved = true;
    m_poll.arm();
  }

private:
  PollEvent m_poll;
  bool m_resolved;
};

/* ── YieldPromiseNode ────────────────────────────────────────────── */

/**
 * @brief Immediately ready but arms breadth-first (next turn).
 */
class YieldPromiseNode final : public PromiseNode {
public:
  void poll(Option<Event &> event) override {
    if (event.is_some()) event.unwrap().arm();
  }

  void read(void *dest) override {
    *static_cast<Void *>(dest) = Void{};
  }
};


/* ── CoroutineEvent (C++20 coroutines) ─────────────────────────── */

#if XPP_HAS_COROUTINES

#include <coroutine>

/**
 * @brief Event that resumes a C++20 coroutine when fired.
 *
 * Bridges coroutine_handle into the event system. When fire() is called
 * (from the event loop), it resumes the suspended coroutine.
 *
 * Allocated by Promise<T>::await_suspend() when a coroutine awaits
 * on a promise. The coroutine_handle is stored and resumed from the
 * event loop callback.
 */
class CoroutineEvent final : public Event {
public:
  explicit CoroutineEvent(std::coroutine_handle<> handle)
      : m_handle(handle) {}

  void fire() override {
    m_fired = true;
    if (m_handle) {
      m_handle.resume();
    }
  }

private:
  std::coroutine_handle<> m_handle;
};

#endif // XPP_HAS_COROUTINES
/* ── maybe_chain helper ──────────────────────────────────────────── */

template <class T>
inline Own<PromiseNode> maybe_chain(Own<PromiseNode> node, T *) {
  return node;
}

template <class T>
Own<PromiseNode> maybe_chain(Own<PromiseNode> node, Promise<T> *);
// Defined after Promise<T> is complete (in promise.h)

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_NODE_H
