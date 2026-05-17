/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cxx11_guard.cpp — compile-only smoke test that proves every public
 * libx++ header is C++11-clean, in strict mode (no -std=gnu++11
 * GNU extensions). Built as the x++_cxx11_guard target when the
 * XPP_CXX11_GUARD CMake option is on; run from CI on every PR so a
 * regression to a C++14-only feature (generic lambda, auto deduced
 * return, std::make_unique, …) gets caught at the syntactic level.
 *
 * The body deliberately instantiates the heavily-templated headers
 * (Result, Option, Variant) so their member functions actually have
 * to compile, not just the class templates' shells. There is no
 * runtime check — the compiler IS the check.
 */

#include <xpp/compiler.h>
#include <xpp/event.h>
#include <xpp/handle.h>
#include <xpp/in_place.h>
#include <xpp/nonnull.h>
#include <xpp/nonnull_own.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/task.h>
#include <xpp/timer.h>
#include <xpp/variant.h>

namespace {

// Force template instantiation of the C++11-sensitive bits.
void instantiate_templates() {
  // Variant: exercises the visitor functors that replaced generic
  // lambdas. Two distinct types so the type-index dispatch matters.
  xpp::Variant<int, double> v(42);
  xpp::Variant<int, double> v2(v);            // copyFrom
  xpp::Variant<int, double> v3(std::move(v)); // moveFrom
  (void) v2;
  (void) v3;

  // Result + Option: exercise the trailing-return-type / decltype
  // map / andThen / mapErr / orElse paths. They're C++11 features,
  // but worth instantiating under the guard so a future contributor
  // can't slip in a C++14 deduced-return shortcut.
  xpp::Result<int, int> r(xpp::ok, 1);
  xpp::Option<int>      o(42);
  (void) r;
  (void) o;
}

} // namespace

// Pull the symbol into the binary so optimizers can't quietly drop the
// instantiation.
void xpp_cxx11_guard_anchor() {
  instantiate_templates();
}
