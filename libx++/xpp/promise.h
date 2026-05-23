/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.h - Promise<T>: composable deferred value.
 *
 * Promise<T> represents a value that will be available in the future.
 * Chain transformations with then(), wait for the result with wait().
 *
 * This is Phase 1 of the async runtime: single-threaded, event-loop
 * driven, with the poll(Event*) interface designed for future co_await
 * integration.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_H
#define XPP_PROMISE_H

#include <xpp/promise_node.h>

#include <utility>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

class WaitScope;
template <class T> class Resolver;

/* ── PromiseForResult ────────────────────────────────────────────── */

template <class Func, class T>
using PromiseForResult = Promise<typename ReducePromise<ReturnType<Func, T>>::Type>;

template <class Func>
using PromiseForResultVoid = Promise<typename ReducePromise<ReturnTypeVoid<Func>>::Type>;

/* ── Promise<T> ──────────────────────────────────────────────────── */

/**
 * @brief A composable deferred value.
 *
 * @code
 *   int result = xpp::eval_later([] { return 42; })
 *     .then([](int x) { return x * 2; })
 *     .wait(scope);
 * @endcode
 *
 * @tparam T  The value type. Use void for completion-only promises.
 */
template <class T> class Promise {
public:
  using ValueType = FixVoid<T>;

  Promise() : m_node(nullptr) {}

  explicit Promise(_::OwnNode node) : m_node(std::move(node)) {}

  Promise(Promise &&o) noexcept : m_node(std::move(o.m_node)) {}
  Promise &operator=(Promise &&o) noexcept {
    m_node = std::move(o.m_node);
    return *this;
  }
  Promise(const Promise &)            = delete;
  Promise &operator=(const Promise &) = delete;

  /**
   * @brief Chain a transformation function.
   *
   * For Promise<T> where T != void: func takes T.
   * For Promise<void>: func takes no args (use this overload).
   *
   * @code
   *   resolve(10).then([](int x) { return x + 1; });
   *   resolve().then([] { return 42; });
   * @endcode
   */
  template <class Func, class V = ValueType,
            class = typename std::enable_if<!std::is_same<V, Void>::value>::type>
  auto then(Func &&func)
    -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type> {
    using RawU     = decltype(std::declval<Func>()(std::declval<V>()));
    using U        = FixVoid<RawU>;
    using ReducedT = typename ReducePromise<RawU>::Type;

    _::OwnNode node(
      new _::TransformPromiseNode<U, V, Func>(std::move(m_node), std::forward<Func>(func)));
    node = _::maybe_chain(std::move(node), static_cast<U *>(nullptr));
    return Promise<ReducedT>(std::move(node));
  }

  /** @brief then() overload for Promise<void> — func takes no args. */
  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type,
            class = void> /* extra param to disambiguate from above */
  auto then(Func &&func)
    -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
    using RawU     = decltype(std::declval<Func>()());
    using U        = FixVoid<RawU>;
    using ReducedT = typename ReducePromise<RawU>::Type;

    _::OwnNode node(
      new _::TransformPromiseNode<U, Void, Func>(std::move(m_node), std::forward<Func>(func)));
    node = _::maybe_chain(std::move(node), static_cast<U *>(nullptr));
    return Promise<ReducedT>(std::move(node));
  }

  /**
   * @brief Synchronously wait for the result, driving the event loop.
   *
   * Blocks the calling thread, running event loop iterations until
   * this promise is resolved. Must be called from the thread that
   * owns the WaitScope.
   *
   * @param scope  The WaitScope providing the event loop to drive.
   * @return       The resolved value.
   */
  T wait(WaitScope &scope);

  /**
   * @brief Discard the value, returning a Promise<void> that completes
   *        when this promise completes.
   */
  Promise<void> discard(); // defined below after Promise<void> is complete

  /** @brief Check if this promise has a node (not moved-from). */
  explicit operator bool() const {
    return m_node != nullptr;
  }

private:
  _::OwnNode m_node;

  template <class U> friend class Promise;
  friend class _::PromiseNode;
  template <class U> friend class Resolver;
};

/* ── Resolver<T> ─────────────────────────────────────────────────── */

/**
 * @brief Producer end of a promise. Call resolve() to fulfill.
 *
 * @code
 *   auto [promise, resolver] = xpp::create_promise<int>();
 *   resolver->resolve(42);
 *   int val = promise.wait(scope);
 * @endcode
 */
template <class T> class Resolver {
public:
  using ValueType = FixVoid<T>;

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

  /**
   * @brief Resolve the promise with a value.
   */
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

/* Specialization for void */
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

/* ── PromisePair ─────────────────────────────────────────────────── */

template <class T> struct PromisePair {
  Promise<T>  promise;
  Resolver<T> resolver;
};

/* ── WaitScope ───────────────────────────────────────────────────── */

/**
 * @brief Provides the event loop context for Promise::wait().
 *
 * Registers the loop into thread-local storage on construction;
 * clears it on destruction. Only one WaitScope per thread.
 *
 * @code
 *   xEventLoop loop = xEventLoopCreate();
 *   xpp::WaitScope scope(loop);
 *   int val = some_promise.wait(scope);
 *   xEventLoopDestroy(loop);
 * @endcode
 */
class WaitScope {
public:
  explicit WaitScope(xEventLoop loop);
  ~WaitScope();

  WaitScope(const WaitScope &)            = delete;
  WaitScope &operator=(const WaitScope &) = delete;

  xEventLoop loop() const {
    return m_loop;
  }

  /** @brief Get the current thread's event loop (or nullptr). */
  static xEventLoop current_loop();

private:
  xEventLoop m_loop;
};

/* ── Factory functions ───────────────────────────────────────────── */

/**
 * @brief Create an already-resolved promise.
 *
 * @code
 *   Promise<int> p = xpp::resolve(42);
 * @endcode
 */
template <class T> Promise<T> resolve(T value) {
  using V = FixVoid<T>;
  _::OwnNode node(new _::ImmediatePromiseNode<V>(std::move(value)));
  return Promise<T>(std::move(node));
}

/** @brief Create an already-resolved Promise<void>. */
inline Promise<void> resolve() {
  _::OwnNode node(new _::ImmediatePromiseNode<Void>(Void{}));
  return Promise<void>(std::move(node));
}

/**
 * @brief Create a promise + resolver pair.
 *
 * @code
 *   auto pair = xpp::create_promise<int>();
 *   pair.resolver.resolve(42);
 *   int val = pair.promise.wait(scope);
 * @endcode
 */
template <class T> PromisePair<T> create_promise() {
  using V            = FixVoid<T>;
  auto      *adapter = new _::AdapterPromiseNode<V>();
  _::OwnNode node(adapter);
  return PromisePair<T>{Promise<T>(std::move(node)), Resolver<T>(adapter)};
}

/**
 * @brief Defer execution to the next event loop turn, then call func.
 *
 * @code
 *   auto p = xpp::eval_later([] { return 42; });
 *   int val = p.wait(scope);  // == 42
 * @endcode
 */
template <class Func> auto eval_later(Func &&func) -> PromiseForResultVoid<Func> {
  return Promise<void>(_::OwnNode(new _::YieldPromiseNode())).then(std::forward<Func>(func));
}

/**
 * @brief Yield control to the event loop, resuming on the next turn.
 *
 * @code
 *   xpp::yield().wait(scope);
 * @endcode
 */
inline Promise<void> yield() {
  return Promise<void>(_::OwnNode(new _::YieldPromiseNode()));
}

/* ── Promise<T>::discard implementation ──────────────────────────── */

template <class T> Promise<void> Promise<T>::discard() {
  return Promise<void>(std::move(m_node));
}

/* ── Promise<T>::wait implementation ─────────────────────────────── */

template <class T> T Promise<T>::wait(WaitScope &scope) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");
  _::RootEvent root;
  m_node->poll(&root);

  while (!root.fired()) {
    xEventWait(scope.loop(), -1);
  }

  ValueType result;
  m_node->read(&result);
  return std::move(result);
}

template <> inline void Promise<void>::wait(WaitScope &scope) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");
  _::RootEvent root;
  m_node->poll(&root);

  while (!root.fired()) {
    xEventWait(scope.loop(), -1);
  }

  Void v;
  m_node->read(&v);
}

/* ── maybe_chain (deferred from promise_node.h) ──────────────────── */

namespace _ {

template <class T> inline OwnNode maybe_chain(OwnNode node, Promise<T> *) {
  return OwnNode(new ChainPromiseNode(std::move(node)));
}

} // namespace _

} // namespace xpp

#endif // XPP_PROMISE_H
