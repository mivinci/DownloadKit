/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * void.h - Void placeholder and FixVoid type mapping.
 *
 * C++ forbids storing, passing, or returning `void` as a value.
 * Void is a unit type that stands in for void in generic code.
 * FixVoid<T>::Type maps T→T, void→Void so templates can uniformly
 * operate on "the value of a Promise<void>" etc.
 *
 * C++11-compatible.
 */

#ifndef XPP_VOID_H
#define XPP_VOID_H

#include <type_traits>
#include <utility>

namespace xpp {

/**
 * @brief Unit type representing void as a storable value.
 */
struct Void {};

template <class T> struct FixVoid { using Type = T; };
template <> struct FixVoid<void> { using Type = Void; };

namespace _voidwrap {

template <class U, class Func>
typename std::enable_if<!std::is_void<decltype(std::declval<Func>()())>::value,
                        typename FixVoid<U>::Type>::type
call(Func &fn) {
  return fn();
}

template <class U, class Func>
typename std::enable_if<std::is_void<decltype(std::declval<Func>()())>::value,
                        typename FixVoid<U>::Type>::type
call(Func &fn) {
  fn();
  return Void{};
}

template <class U, class T, class Func>
typename std::enable_if<!std::is_void<decltype(std::declval<Func>()(std::declval<T>()))>::value,
                        typename FixVoid<U>::Type>::type
call1(Func &fn, T &&arg) {
  return fn(std::forward<T>(arg));
}

template <class U, class T, class Func>
typename std::enable_if<std::is_void<decltype(std::declval<Func>()(std::declval<T>()))>::value,
                        typename FixVoid<U>::Type>::type
call1(Func &fn, T &&arg) {
  fn(std::forward<T>(arg));
  return Void{};
}

} // namespace _voidwrap

} // namespace xpp

#endif // XPP_VOID_H
