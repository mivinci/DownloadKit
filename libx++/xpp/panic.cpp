/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * panic.cpp - Implementation of xpp::_::doPanic.
 *
 * Kept out of panic.h so the logging-library dependency stays here
 * instead of leaking into every TU that uses XPP_PANIC / XPP_ASSERT
 * through a header (which is every TU that touches Option / Result /
 * NonNull / Rc / …, since their *_assert paths reach the macros).
 *
 * Routing today: forward to libx's xLogV(fatal=true), which prints
 * the message, collects a backtrace, and aborts. Swap in a different
 * routing here without touching any other TU.
 */

#include <xpp/panic.h>

#include <cstdarg>
#include <cstdlib>

#include <x/base/log.h>

namespace xpp {
namespace _ {

void doPanic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  xLogV(/*fatal=*/true, fmt, ap);
  va_end(ap);
  // xLogV(fatal=true) calls abort() and never returns. The std::abort()
  // below is unreachable but satisfies XPP_NORETURN on every code path.
  std::abort();
}

} // namespace _
} // namespace xpp
