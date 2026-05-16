/*
 * Copyright 2025 The libxpp Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * in_place.h - Tag types for in-place construction (cf. std::in_place_*).
 */

#ifndef XPP_IN_PLACE_H
#define XPP_IN_PLACE_H

#include <cstddef>

namespace xpp {

/**
 * @brief Index-based tag for in-place constructing the N-th alternative.
 *
 * Analogous to std::in_place_index_t<N>. Used to disambiguate constructor
 * overloads when types alone are insufficient (e.g. OneOf<int, int>).
 *
 * Usage:
 *   OneOf<int, int> a(InPlaceIndex<0>{}, 42);  // first int
 *   OneOf<int, int> b(InPlaceIndex<1>{}, 42);  // second int
 */
template <size_t N> struct InPlaceIndex {
  static constexpr size_t kValue = N;
};

} // namespace xpp

#endif // XPP_IN_PLACE_H
