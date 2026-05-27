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

#include <xpp/arc.h>
#include <xpp/compiler.h>
#include <xpp/sys/cond.h>
#include <xpp/error.h>
#include <xpp/handle.h>
#include <xpp/in_place.h>
#include <xpp/sys/mutex.h>
#include <xpp/nonnull.h>
#include <xpp/box.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/rc.h>
#include <xpp/result.h>
#include <xpp/variant.h>
#include <xpp/weak.h>

namespace {

// Force template instantiation of the C++11-sensitive bits.
void instantiate_templates() {
  // Variant: exercises the visitor functors that replaced generic
  // lambdas. Two distinct types so the type-index dispatch matters.
  xpp::Variant<int, double> v(42);
  xpp::Variant<int, double> v2(v);            // copy_from
  xpp::Variant<int, double> v3(std::move(v)); // move_from
  (void) v2;
  (void) v3;

  // Result + Option: exercise the trailing-return-type / decltype
  // map / and_then / map_err / or_else paths. They're C++11 features,
  // but worth instantiating under the guard so a future contributor
  // can't slip in a C++14 deduced-return shortcut.
  xpp::Result<int, int> r(xpp::ok, 1);
  xpp::Option<int>      o(42);
  (void) r;
  (void) o;

  // Error + Result<void, Error>: the canonical libx++ error channel.
  // Round-trips a code through Error and back so the int-like
  // contract gets compiled.
  xpp::Error             e{42};
  xpp::Result<void, xpp::Error> rv(xpp::err, e);
  (void) e.code();
  (void) rv;

  // Rc + Option<Rc> + Weak: exercises Rc::make, copy ctor (+1), the
  // niche-optimised Option<Rc<T>> specialisation, and the Weak ↔ Rc
  // bridging via downgrade()/upgrade(). Drops back to None at scope
  // exit so the runtime path also runs.
  xpp::Rc<int>              r1 = xpp::Rc<int>::make(7);
  xpp::Rc<int>              r2 = r1.clone();
  xpp::Option<xpp::Rc<int>> opt(r1);
  xpp::Weak<int>            w  = xpp::Rc<int>::downgrade(r1);
  xpp::Option<xpp::Rc<int>> up = w.upgrade();
  (void) r2;
  (void) opt;
  (void) up;

  // Arc + Option<Arc> + ArcWeak: the atomic counterpart. Same
  // operations, std::atomic counters under the hood.
  xpp::Arc<int>              a1 = xpp::Arc<int>::make(9);
  xpp::Arc<int>              a2 = a1.clone();
  xpp::Option<xpp::Arc<int>> aopt(a1);
  xpp::ArcWeak<int>          aw  = xpp::Arc<int>::downgrade(a1);
  xpp::Option<xpp::Arc<int>> aup = aw.upgrade();
  (void) a2;
  (void) aopt;
  (void) aup;

  // Mutex + Condvar: data + lock fusion plus condvar companion.
  // Variadic ctor forwarding; MutexGuard's operator->/operator*/get;
  // try_lock returning Option<MutexGuard>. Condvar::wait /
  // wait_timeout / notify_one / notify_all all parse + instantiate.
  xpp::sys::Mutex<int> mtx(11);
  {
    xpp::sys::MutexGuard<int> g = mtx.lock();
    *g                     = 13;
    int &r                 = g.get();
    (void) r;
  }
  xpp::Option<xpp::sys::MutexGuard<int>> tg = mtx.try_lock();
  (void) tg;
  xpp::sys::Condvar cnd;
  cnd.notify_one();
  cnd.notify_all();
}

} // namespace

// Pull the symbol into the binary so optimizers can't quietly drop the
// instantiation.
void xpp_cxx11_guard_anchor() {
  instantiate_templates();
}
