/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.h - Promise<T>: composable deferred value.
 *
 * Design inspired by KJ (Cap'n Proto async library):
 *   https://github.com/capnproto/capnproto/blob/master/c%2B%2B/src/kj/async.h
 *
 * Promise<T> represents a value that will be available in the future.
 * Chain transformations with then(), wait for the result with wait().
 *
 * This is Phase 1 of the async runtime: single-threaded, event-loop
 * driven, with the poll(Waker) interface designed for future co_await
 * integration.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_H
#define XPP_PROMISE_H

#include <xpp/compiler.h>
#include <xpp/promise_node.h>

#include <utility>

extern "C" {
#include <x/base/event.h>
}

#if XPP_HAS_COROUTINES
#include <coroutine>
#endif

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

class EnterGuard;
template <class T> class Resolver;
template <class T> struct PromiseAndResolver;

xEventLoop current_event_loop();

/* ── PromiseForResult ────────────────────────────────────────────── */

template <class Func, class T>
using PromiseForResult = Promise<typename ReducePromise<ReturnType<Func, T>>::Type>;

template <class Func>
using PromiseForResultVoid = Promise<typename ReducePromise<ReturnTypeVoid<Func>>::Type>;

/* ── Promise<T> ──────────────────────────────────────────────────── */

/**
 * @brief A composable deferred value.
 * @tparam T  The value type. Use void for completion-only promises.
 *
 * @code
 *   int result = xpp::Promise<int>::eval([] { return 42; })
 *     .then([](int x) { return x * 2; })
 *     .wait(scope);
 * @endcode
 */
template <class T> class Promise {
public:
  using ValueType = typename FixVoid<T>::Type;

#if XPP_HAS_COROUTINES
  /* ── Coroutine promise_type ─────────────────────────────────────── */

  struct promise_type {
    _::AdapterPromiseNode<ValueType> *adapter;

    promise_type() {
      adapter = new _::AdapterPromiseNode<ValueType>();
    }

    Promise get_return_object() {
      return Promise(Own<_::PromiseNode<T>>(adapter));
    }

    std::suspend_never initial_suspend() noexcept {
      return {};
    }
    std::suspend_never final_suspend() noexcept {
      return {};
    }

    void return_value(ValueType value) {
      adapter->resolve(std::move(value));
    }

    void unhandled_exception() {
      XPP_PANIC("unhandled exception in coroutine returning Promise<T>");
    }
  };
#endif // XPP_HAS_COROUTINES

  Promise() : m_node(nullptr) {}
  explicit Promise(Own<_::PromiseNode<T>> node) : m_node(std::move(node)) {}
  Promise(Promise &&o) noexcept : m_node(std::move(o.m_node)) {}
  Promise &operator=(Promise &&o) noexcept {
    m_node = std::move(o.m_node);
    return *this;
  }
  Promise(const Promise &)            = delete;
  Promise &operator=(const Promise &) = delete;

  template <class Func, class V = ValueType,
            class = typename std::enable_if<!std::is_same<V, Void>::value>::type>
  auto then(Func &&func)
    -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type>;

  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type, class = void>
  auto then(Func &&func) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type>;

  T wait(EnterGuard &guard);

  Promise<void> discard();

#if XPP_HAS_COROUTINES
  auto operator co_await() {
    struct Awaiter {
      _::PromiseNode<T> *node;

      bool await_ready() const {
        return false;
      }

      bool await_suspend(std::coroutine_handle<> h) {
        _::Schedule *sched = new _::CoroWakeSchedule(h, xpp::current_event_loop());
        _::Waker w(sched, nullptr);
        if (node->poll(w)) {
          w.wake();
        }
        return true;
      }

      auto await_resume() -> typename _::PromiseNode<T>::ValueType {
        return node->take();
      }
    };
    XPP_ASSERT(m_node != nullptr, "co_await on empty promise");
    return Awaiter{m_node.get()};
  }
#endif

  /* ── Static factories ───────────────────────────────────────────── */

  static Promise resolve(ValueType value) {
    Own<_::PromiseNode<T>> node(new _::ImmediatePromiseNode<T>(std::move(value)));
    return Promise(std::move(node));
  }

  template <class V = ValueType,
            class   = typename std::enable_if<std::is_same<V, Void>::value>::type>
  static Promise resolve() {
    Own<_::PromiseNode<T>> node(new _::ImmediatePromiseNode<void>());
    return Promise(std::move(node));
  }

  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type>
  static auto eval(Func &&func) -> Promise<typename ReducePromise<ReturnTypeVoid<Func>>::Type> {
    return Promise<void>(Own<_::PromiseNode<void>>(new _::YieldPromiseNode()))
      .then(std::forward<Func>(func));
  }

  static PromiseAndResolver<T> make();

  explicit operator bool() const {
    return m_node != nullptr;
  }

  Own<_::PromiseNode<T>> release_node() {
    return std::move(m_node);
  }

private:
  Own<_::PromiseNode<T>> m_node;

  template <class U> friend class Promise;
  template <class U> friend class Resolver;
  template <class U> friend class JoinHandle;
  template <class U> friend class _::ChainPromiseNode;
};

/* ── Resolver<T> ─────────────────────────────────────────────────── */

template <class T> class Resolver {
public:
  using ValueType = typename FixVoid<T>::Type;

  explicit Resolver(_::AdapterPromiseNode<ValueType> *node) : m_node(node) {}

  Resolver(Resolver &&o) noexcept : m_node(o.m_node) {
    o.m_node = nullptr;
  }
  Resolver &operator=(Resolver &&o) noexcept {
    m_node   = o.m_node;
    o.m_node = nullptr;
    return *this;
  }
  Resolver(const Resolver &)            = delete;
  Resolver &operator=(const Resolver &) = delete;

  void resolve(ValueType &&value) {
    XPP_ASSERT(m_node != nullptr, "Resolver: already consumed or moved-from");
    m_node->resolve(std::move(value));
    m_node = nullptr;
  }

  void resolve(const ValueType &value) {
    resolve(ValueType(value));
  }

  bool is_pending() const {
    return m_node != nullptr;
  }

private:
  _::AdapterPromiseNode<ValueType> *m_node;
};

template <> class Resolver<void> {
public:
  explicit Resolver(_::AdapterPromiseNode<Void> *node) : m_node(node) {}

  Resolver(Resolver &&o) noexcept : m_node(o.m_node) {
    o.m_node = nullptr;
  }
  Resolver &operator=(Resolver &&o) noexcept {
    m_node   = o.m_node;
    o.m_node = nullptr;
    return *this;
  }

  void resolve() {
    XPP_ASSERT(m_node != nullptr, "Resolver: already consumed or moved-from");
    m_node->resolve();
    m_node = nullptr;
  }

  bool is_pending() const {
    return m_node != nullptr;
  }

private:
  _::AdapterPromiseNode<Void> *m_node;
};

/* ── PromiseAndResolver ─────────────────────────────────────────────────── */

template <class T> struct PromiseAndResolver {
  Promise<T>  promise;
  Resolver<T> resolver;
};

/* ── Promise<T>::make ──────────────────────────────────────────────── */

template <class T> PromiseAndResolver<T> Promise<T>::make() {
  auto                  *adapter = new _::AdapterPromiseNode<ValueType>();
  Own<_::PromiseNode<T>> node(adapter);
  return PromiseAndResolver<T>{Promise(std::move(node)), Resolver<T>(adapter)};
}

/* ── Free helper functions ──────────────────────────────────────── */

inline Promise<void> yield() {
  return Promise<void>(Own<_::PromiseNode<void>>(new _::YieldPromiseNode()));
}

/* ── Promise<T>::discard implementation ──────────────────────────── */

template <class T> Promise<void> Promise<T>::discard() {
  return then([](ValueType) {});
}

/* ── Promise<T>::wait (declared only; defined in runtime.h) ─────── */

/* ── C++20 Coroutine support for Promise<void> ──────────────────── */

#if XPP_HAS_COROUTINES

template <> struct Promise<void>::promise_type {
  _::AdapterPromiseNode<Void> *adapter;

  promise_type() {
    adapter = new _::AdapterPromiseNode<Void>();
  }

  Promise<void> get_return_object() {
    return Promise<void>(Own<_::PromiseNode<void>>(adapter));
  }

  std::suspend_never initial_suspend() noexcept {
    return {};
  }
  std::suspend_never final_suspend() noexcept {
    return {};
  }

  void return_void() {
    adapter->resolve();
  }

  void unhandled_exception() {
    std::terminate();
  }
};

#endif // XPP_HAS_COROUTINES

/* ── ChainPromiseNode<T> implementation ─────────────────────────── */

namespace _ {

template <class T>
inline Own<PromiseNode<T>> maybe_chain(Own<PromiseNode<Promise<T>>> node, Promise<T> *) {
  return Own<PromiseNode<T>>(new ChainPromiseNode<T>(std::move(node)));
}

/* ── ChainPromiseNode<T> implementation ─────────────────────────── */

template <class T>
ChainPromiseNode<T>::ChainPromiseNode(Own<PromiseNode<Promise<T>>> outer)
    : m_state(Step1), m_outer(std::move(outer)) {}

template <class T> bool ChainPromiseNode<T>::poll(Waker waker) {
  switch (m_state) {
  case Step1:
    if (!m_outer->poll(waker)) return false;
    m_inner = std::move(m_outer->take().m_node);
    m_outer = nullptr;
    m_state = Step2;
    return m_inner->poll(waker);
  case Step2:
    return m_inner->poll(waker);
  }
  XPP_UNREACHABLE();
}

template <class T> typename PromiseNode<T>::ValueType ChainPromiseNode<T>::take() {
  XPP_ASSERT(m_state == Step2, "ChainPromiseNode::take in Step1");
  return m_inner->take();
}

/* ── chain helper: always returns Own<PromiseNode<ReducedT>> ── */

namespace _chain {

template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<std::is_same<OutT, ReducedT>::value, Own<PromiseNode<ReducedT>>>::type
chain(Own<PromiseNode<T>> dep, Func &&func) {
  return Own<PromiseNode<ReducedT>>(
    new TransformPromiseNode<ReducedT, T, Func>(std::move(dep), std::forward<Func>(func)));
}

template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<!std::is_same<OutT, ReducedT>::value, Own<PromiseNode<ReducedT>>>::type
chain(Own<PromiseNode<T>> dep, Func &&func) {
  return Own<PromiseNode<ReducedT>>(new ChainPromiseNode<ReducedT>(
    Own<PromiseNode<Promise<ReducedT>>>(new TransformPromiseNode<Promise<ReducedT>, T, Func>(
      std::move(dep), std::forward<Func>(func)))));
}

} // namespace _chain

} // namespace _

/* ── Promise<T>::then implementations ──────────────────────────── */

template <class T>
template <class Func, class V, class>
auto Promise<T>::then(Func &&func)
  -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type> {
  using RawU     = decltype(std::declval<Func>()(std::declval<V>()));
  using ReducedT = typename ReducePromise<RawU>::Type;
  using OutT     = RawU;

  Own<_::PromiseNode<T>> dep(std::move(m_node));
  auto node = _::_chain::chain<ReducedT, OutT, T, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

template <class T>
template <class Func, class V, class, class>
auto Promise<T>::then(Func &&func)
  -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
  using RawU     = decltype(std::declval<Func>()());
  using ReducedT = typename ReducePromise<RawU>::Type;
  using OutT     = RawU;

  Own<_::PromiseNode<void>> dep(std::move(m_node));
  auto                      node =
    _::_chain::chain<ReducedT, OutT, void, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

} // namespace xpp

#endif // XPP_PROMISE_H
