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

#include <xpp/own.h>
#include <xpp/panic.h>

#include <cstddef>
#include <type_traits>
#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

template <class T> class Promise;

/* ── Void placeholder ────────────────────────────────────────────── */

struct Void {};

template <class T> struct FixVoid_ { using Type = T; };
template <> struct FixVoid_<void> { using Type = Void; };
template <class T> using FixVoid = typename FixVoid_<T>::Type;

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
  PollEvent() : m_event(nullptr), m_ready(false) {}

  void init(Event *event) {
    if (m_ready) {
      if (event) event->arm();
    } else {
      m_event = event;
    }
  }

  void arm() {
    m_ready = true;
    if (m_event) {
      m_event->arm();
      m_event = nullptr;
    }
  }

  bool is_ready() const { return m_ready; }

private:
  Event *m_event;
  bool m_ready;
};

/* ── PromiseNode ─────────────────────────────────────────────────── */

class PromiseNode;
using OwnNode = Own<PromiseNode>;

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
   * If already ready, arm immediately. The event pointer is stored
   * (not owned) and must outlive the node or be disarmed.
   *
   * @param event  Event to arm, or nullptr to just check readiness.
   */
  virtual void poll(Event *event) = 0;

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

  void poll(Event *event) override {
    if (event) event->arm();
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
 * @tparam U     Output type (FixVoid<ReturnType<Func, T>>)
 * @tparam T     Input type from dependency
 * @tparam Func  Callable T → U
 */
template <class U, class T, class Func>
class TransformPromiseNode final : public PromiseNode {
public:
  TransformPromiseNode(OwnNode dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Event *event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    T dep_value;
    m_dep->read(&dep_value);
    *static_cast<U *>(dest) = m_func(std::move(dep_value));
  }

private:
  OwnNode m_dep;
  Func m_func;
};

/* Specialization: T=Void (dependency is Promise<void>) */
template <class U, class Func>
class TransformPromiseNode<U, Void, Func> final : public PromiseNode {
public:
  TransformPromiseNode(OwnNode dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Event *event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    Void v;
    m_dep->read(&v);
    *static_cast<U *>(dest) = m_func();
  }

private:
  OwnNode m_dep;
  Func m_func;
};

/* Specialization: U=Void (func returns void) */
template <class T, class Func>
class TransformPromiseNode<Void, T, Func> final : public PromiseNode {
public:
  TransformPromiseNode(OwnNode dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Event *event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    T dep_value;
    m_dep->read(&dep_value);
    m_func(std::move(dep_value));
    *static_cast<Void *>(dest) = Void{};
  }

private:
  OwnNode m_dep;
  Func m_func;
};

/* Specialization: both Void → Void */
template <class Func>
class TransformPromiseNode<Void, Void, Func> final : public PromiseNode {
public:
  TransformPromiseNode(OwnNode dep, Func &&func)
      : m_dep(std::move(dep)), m_func(std::move(func)) {}

  void poll(Event *event) override {
    m_dep->poll(event);
  }

  void read(void *dest) override {
    Void v;
    m_dep->read(&v);
    m_func();
    *static_cast<Void *>(dest) = Void{};
  }

private:
  OwnNode m_dep;
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
  explicit ChainPromiseNode(OwnNode inner)
      : m_state(Step1), m_inner(std::move(inner)), m_outer_event(nullptr) {
    m_inner->poll(this);
  }

  void poll(Event *event) override {
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
  OwnNode m_inner;
  Event *m_outer_event;
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

  void poll(Event *event) override {
    if (m_resolved) {
      if (event) event->arm();
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

  void poll(Event *event) override {
    if (m_resolved) {
      if (event) event->arm();
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
  void poll(Event *event) override {
    if (event) event->arm();
  }

  void read(void *dest) override {
    *static_cast<Void *>(dest) = Void{};
  }
};

/* ── maybe_chain helper ──────────────────────────────────────────── */

template <class T>
inline OwnNode maybe_chain(OwnNode node, T *) {
  return node;
}

template <class T>
OwnNode maybe_chain(OwnNode node, Promise<T> *);
// Defined after Promise<T> is complete (in promise.h)

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_NODE_H
