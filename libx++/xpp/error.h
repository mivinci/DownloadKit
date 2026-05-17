/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - xpp::Error, the libx++ recoverable-error value type.
 *
 * Lives at the top of xpp/ on purpose: every libx++ submodule
 * (base/, future net/, http/, agent/, …) returns Result<T, Error>,
 * so the type belongs to the whole library, not to any one module.
 *
 * Error itself does not know about xErrno or any other C-side error
 * vocabulary. It just holds a small int-like `code` plus equality.
 * Each submodule's bridging helper builds an Error from whatever
 * error code its underlying C API speaks — xErrno today, an HTTP
 * status when xpp/http/ lands, etc. The Error reaching the caller
 * compares and inspects identically regardless of where it came
 * from, and a header that uses Result<T, Error> is not forced to
 * include the originating C error header.
 *
 * Bridging at the C/C++ boundary (lives in the submodule's .cpp,
 * NOT in this header):
 *
 *   // base/event.cpp
 *   inline Result<void, Error> from_errno(xErrno rc) {
 *     if (rc == xErrno_Ok) return Result<void, Error>(ok);
 *     return Result<void, Error>(err, Error{static_cast<int>(rc)});
 *   }
 *
 *   // C++ caller
 *   auto r = loop.wait(100);
 *   if (r.isErr() && r.unwrapErr().code() == xErrno_Timeout) { ... }
 *
 * The caller's `e.code() == xErrno_Timeout` line is the place where
 * the C-side vocabulary re-enters explicitly; nothing about Error
 * forces that comparison to use xErrno. A caller could equally
 * compare against HTTP-status constants when inspecting an error
 * surfaced from xpp/http/.
 *
 * Future evolution (a category enum, an optional message, a source
 * location, a richer encoding) extends Error without breaking any
 * existing Result<T, Error> caller.
 *
 * Header-only. C++11-compatible. Trivially copyable (sizeof(int)).
 */

#ifndef XPP_ERROR_H
#define XPP_ERROR_H

namespace xpp {

/**
 * @brief Recoverable-error value type used by Result<T, Error>.
 *
 * Trivially copyable; pass by value. Holds a small int-like code
 * whose meaning is owned by the submodule that produced it. Compare
 * with Error::operator== for "same error?" semantics; reach for
 * code() when you need to switch on the specific failure.
 */
class Error {
public:
  /** The raw code type. Wide enough for any libx-family error code. */
  using Code = int;

  /**
   * @brief Construct from a raw error code.
   *
   * Marked explicit so a stray int doesn't silently become an Error;
   * the bridging point is always a deliberate `Error{code}`. Submodule
   * helpers (e.g. xpp::from_errno in base/) static_cast their C-side
   * code into this constructor.
   */
  explicit Error(Code code) noexcept : m_code(code) {}

  /**
   * @brief Underlying error code.
   *
   * Callers that need to switch on the specific failure compare this
   * against the appropriate C-side constant — xErrno_Timeout for an
   * Error from xpp/base/, an HTTP status for xpp/http/, etc.
   */
  Code code() const noexcept {
    return m_code;
  }

  bool operator==(const Error &o) const noexcept {
    return m_code == o.m_code;
  }
  bool operator!=(const Error &o) const noexcept {
    return m_code != o.m_code;
  }

private:
  Code m_code;
};

} // namespace xpp

#endif // XPP_ERROR_H
