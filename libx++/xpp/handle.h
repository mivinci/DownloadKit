/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * handle.h - Move-only RAII handle base template.
 */

#ifndef XPP_HANDLE_H
#define XPP_HANDLE_H

namespace xpp {

/**
 * @brief Tag type for constructing a Handle from a raw C pointer.
 *
 * Needed because all libx opaque handles are void*, so overloads
 * like Handle(xTimer) and Handle(xTaskGroup) would be ambiguous.
 * Usage: Handle(xTimerCreate(...), FromRaw{})
 */
struct FromRaw {};

/**
 * @brief Move-only RAII handle that calls Drop(h) on destruction.
 *
 * @tparam T     Opaque C handle type (e.g. xEventLoop, xTimer).
 * @tparam Drop  Function pointer: void(*)(T) — called in the destructor.
 *
 * Provides the boilerplate that every xpp wrapper shares:
 *   - Destructor that releases the handle
 *   - Move constructor / assignment (source nulled)
 *   - Deleted copy
 *   - handle() accessor + bool conversion
 */
template <typename T, void (*Drop)(T)> class Handle {
public:
  /** @brief Default-constructs a null handle. */
  constexpr Handle() noexcept = default;

  /**
   * @brief Construct from a raw C handle.
   * @param h  The raw handle to take ownership of.
   */
  constexpr Handle(T h, FromRaw) noexcept : m_h(h) {}

  /** @brief Destructor. Calls Drop(m_h) if the handle is non-null. */
  ~Handle() {
    if (m_h) Drop(m_h);
  }

  /**
   * @brief Move constructor.
   *
   * Transfers ownership; the source is left null.
   *
   * @param o  Source handle (left null after move).
   */
  Handle(Handle &&o) noexcept : m_h(o.m_h) {
    o.m_h = nullptr;
  }

  /**
   * @brief Move assignment.
   *
   * Releases the current handle (if any), then transfers ownership
   * from @p o. The source is left null.
   *
   * @param o  Source handle (left null after move).
   * @return   Reference to this.
   */
  Handle &operator=(Handle &&o) noexcept {
    if (this != &o) {
      if (m_h) Drop(m_h);
      m_h   = o.m_h;
      o.m_h = nullptr;
    }
    return *this;
  }

  Handle(const Handle &)            = delete;
  Handle &operator=(const Handle &) = delete;

  /**
   * @brief Access the underlying C handle.
   * @return The raw handle, or nullptr if empty.
   */
  constexpr T handle() const noexcept {
    return m_h;
  }

  /**
   * @brief Check whether the handle is non-null.
   * @return True if the handle holds a valid (non-null) pointer.
   */
  constexpr explicit operator bool() const noexcept {
    return m_h != nullptr;
  }

protected:
  T m_h = nullptr;
};

} // namespace xpp

#endif // XPP_HANDLE_H
