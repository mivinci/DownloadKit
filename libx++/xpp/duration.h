/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * duration.h - Duration: a non-negative span of time.
 *
 * Mirrors Rust's std::time::Duration as closely as C++11 allows:
 *
 *   - non-negative ("a length, not an offset"); a Duration cannot
 *     represent "5 ms ago". For "t1 - t2 might be in the past",
 *     reach for checked_sub / saturating_sub or, when Time lands,
 *     Time::saturating_duration_since;
 *   - explicit construction — Duration(5) does *not* compile, you
 *     write 5_ms or Duration::from_millis(5). This is the whole
 *     reason Rust picked this shape: the type prevents "is 5 a
 *     nanosecond, a millisecond, or a second?" at every call site;
 *   - overflow is a contract violation, not silent wrap. The plain
 *     operators (+ - *) panic on overflow / underflow; the
 *     checked* / saturating* family give recoverable variants.
 *
 * Storage is a single uint64_t of nanoseconds. That gives a max
 * span of ~584 years, which is enough headroom for every realistic
 * use of Duration in this codebase (mutex / cond timeouts, RPC
 * deadlines, scheduler ticks). Rust uses u64 secs + u32 nanos for
 * ~584 billion years because std::time::SystemTime piggy-backs on
 * Duration to express far-future timestamps; we don't have that
 * constraint here, and a single u64 keeps sizeof(Duration) at 8
 * with one less arithmetic op per accessor.
 *
 * The literal suffixes (_ns / _us / _ms / _s / _min / _h) live in
 * xpp::literals so a TU only opts in by `using namespace
 * xpp::literals;`. Same pattern as std::chrono_literals.
 *
 * Header-only and libx-free; safe to include anywhere.
 *
 * C++11-compatible.
 */

#ifndef XPP_DURATION_H
#define XPP_DURATION_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <cstdint>

namespace xpp {

namespace _ {

/* ── Internal helpers ───────────────────────────────────────────────
 *
 * Constants kept here (rather than as Duration static members) so
 * the Duration class body reads as API only. Same convention as
 * mutex.h's k_mutex_storage_size.
 */
constexpr uint64_t k_nanos_per_micro = 1000ULL;
constexpr uint64_t k_nanos_per_milli = 1000000ULL;
constexpr uint64_t k_nanos_per_sec   = 1000000000ULL;
constexpr uint64_t k_nanos_per_min   = 60ULL * 1000000000ULL;
constexpr uint64_t k_nanos_per_hour  = 3600ULL * 1000000000ULL;

/* The largest u64 nanosecond count, exposed so Duration::max() can
 * be constexpr without naming the underlying type. */
constexpr uint64_t k_duration_max_nanos = static_cast<uint64_t>(-1);

} // namespace _

/**
 * @brief A non-negative span of time, with nanosecond resolution
 *        and a maximum value of ~584 years.
 *
 * Construction is deliberately verbose:
 *
 *   auto d1 = Duration::from_millis(250);   // factory
 *   auto d2 = 250_ms;                      // literal (after
 *                                          // `using namespace
 *                                          //  xpp::literals;`)
 *   auto d3 = Duration::zero();            // 0
 *   auto d4 = Duration::max();             // ~584 years
 *
 * Implicit construction from an integer is forbidden — `5` is
 * neither obviously nanoseconds nor obviously milliseconds.
 *
 * Arithmetic:
 *
 *   a + b, a - b           panic on overflow / a < b;
 *   a * k, k * a, a / k    panic on overflow / k == 0;
 *   a / b   -> double      ratio (lossy);
 *   a += b, a -= b, ...    in-place, same panic rules;
 *
 *   a.checked_add(b)        -> Option<Duration> (none on overflow);
 *   a.saturating_add(b)     -> Duration         (clamped at max());
 *   a.checked_sub(b)        -> Option<Duration> (none if b > a);
 *   a.saturating_sub(b)     -> Duration         (clamped at zero());
 *
 * Duration * Duration is intentionally not provided: nanoseconds
 * times nanoseconds is a "nanosecond squared", which has no useful
 * meaning. Multiply Duration by a scalar instead.
 *
 * Trivially copyable; pass by value.
 */
class Duration {
public:
  /** @brief Zero duration. Default-constructed Durations are zero. */
  constexpr Duration() noexcept : m_nanos(0) {}

  /**
   * @brief Construct directly from a nanosecond count.
   *
   * `explicit` to keep `Duration(5)` from sneaking past readers as
   * "5 of something". Prefer from_xxx() factories or _xx literals
   * at every call site that isn't internal plumbing.
   */
  explicit constexpr Duration(uint64_t nanos) noexcept : m_nanos(nanos) {}

  /* ── Factories ─────────────────────────────────────────────── */

  /** @brief A Duration of zero. */
  static constexpr Duration zero() noexcept {
    return Duration(0);
  }

  /** @brief The largest representable Duration (~584 years). */
  static constexpr Duration max() noexcept {
    return Duration(_::k_duration_max_nanos);
  }

  /** @brief Construct from a nanosecond count. */
  static constexpr Duration from_nanos(uint64_t n) noexcept {
    return Duration(n);
  }

  /**
   * @brief Construct from a microsecond count.
   *
   * Panics on overflow (n > k_duration_max_nanos / 1000, ~5.8e14).
   */
  static Duration from_micros(uint64_t n) noexcept {
    XPP_ASSERT(n <= _::k_duration_max_nanos / _::k_nanos_per_micro,
               "Duration::from_micros overflow: n=%llu", static_cast<unsigned long long>(n));
    return Duration(n * _::k_nanos_per_micro);
  }

  /** @brief Construct from a millisecond count. Panics on overflow. */
  static Duration from_millis(uint64_t n) noexcept {
    XPP_ASSERT(n <= _::k_duration_max_nanos / _::k_nanos_per_milli,
               "Duration::from_millis overflow: n=%llu", static_cast<unsigned long long>(n));
    return Duration(n * _::k_nanos_per_milli);
  }

  /** @brief Construct from a (whole-)second count. Panics on overflow. */
  static Duration from_secs(uint64_t n) noexcept {
    XPP_ASSERT(n <= _::k_duration_max_nanos / _::k_nanos_per_sec,
               "Duration::from_secs overflow: n=%llu", static_cast<unsigned long long>(n));
    return Duration(n * _::k_nanos_per_sec);
  }

  /** @brief Construct from a minute count. Panics on overflow. */
  static Duration from_mins(uint64_t n) noexcept {
    XPP_ASSERT(n <= _::k_duration_max_nanos / _::k_nanos_per_min,
               "Duration::from_mins overflow: n=%llu", static_cast<unsigned long long>(n));
    return Duration(n * _::k_nanos_per_min);
  }

  /** @brief Construct from an hour count. Panics on overflow. */
  static Duration from_hours(uint64_t n) noexcept {
    XPP_ASSERT(n <= _::k_duration_max_nanos / _::k_nanos_per_hour,
               "Duration::from_hours overflow: n=%llu", static_cast<unsigned long long>(n));
    return Duration(n * _::k_nanos_per_hour);
  }

  /**
   * @brief Construct from a fractional second count.
   *
   * Recoverable variant: returns None for negative, NaN, infinite,
   * or out-of-range inputs. Mirrors Rust's
   * Duration::try_from_secs_f64.
   *
   * @param secs  Seconds as a double; e.g. 1.5 -> 1500 ms.
   */
  static Option<Duration> try_from_secs_f64(double secs) noexcept {
    // Reject NaN, Inf, negatives.
    if (!(secs >= 0.0)) return none; // NB: covers NaN.
    const double nanos = secs * static_cast<double>(_::k_nanos_per_sec);
    // Reject anything that won't fit in u64. 1.8e19 is the boundary;
    // use < (not <=) because the cast of the boundary itself is UB.
    if (nanos >= 1.8446744073709552e19) return none;
    return Some(Duration(static_cast<uint64_t>(nanos)));
  }

  /* ── Accessors ─────────────────────────────────────────────── */

  /** @brief Total nanoseconds. */
  constexpr uint64_t as_nanos() const noexcept {
    return m_nanos;
  }

  /** @brief Total whole microseconds (truncates). */
  constexpr uint64_t as_micros() const noexcept {
    return m_nanos / _::k_nanos_per_micro;
  }

  /** @brief Total whole milliseconds (truncates). */
  constexpr uint64_t as_millis() const noexcept {
    return m_nanos / _::k_nanos_per_milli;
  }

  /** @brief Total whole seconds (truncates). */
  constexpr uint64_t as_secs() const noexcept {
    return m_nanos / _::k_nanos_per_sec;
  }

  /** @brief Total seconds as a double (lossy at large values). */
  constexpr double as_secs_f64() const noexcept {
    return static_cast<double>(m_nanos) / static_cast<double>(_::k_nanos_per_sec);
  }

  /**
   * @brief Sub-second nanoseconds — i.e. m_nanos mod 1e9.
   *
   * Together with as_secs(), this lets you fill a struct timespec
   * without a second division.
   */
  constexpr uint32_t subsec_nanos() const noexcept {
    return static_cast<uint32_t>(m_nanos % _::k_nanos_per_sec);
  }

  /** @brief True iff this Duration is exactly zero. */
  constexpr bool is_zero() const noexcept {
    return m_nanos == 0;
  }

  /* ── Checked arithmetic ───────────────────────────────────── */

  /** @brief a + b; None on overflow. */
  Option<Duration> checked_add(Duration o) const noexcept {
    if (m_nanos > _::k_duration_max_nanos - o.m_nanos) return none;
    return Some(Duration(m_nanos + o.m_nanos));
  }

  /** @brief a - b; None if b > a. */
  Option<Duration> checked_sub(Duration o) const noexcept {
    if (m_nanos < o.m_nanos) return none;
    return Some(Duration(m_nanos - o.m_nanos));
  }

  /** @brief a * k; None on overflow. k is unsigned to avoid sign games. */
  Option<Duration> checked_mul(uint64_t k) const noexcept {
    if (k == 0 || m_nanos == 0) return Some(Duration(0));
    if (m_nanos > _::k_duration_max_nanos / k) return none;
    return Some(Duration(m_nanos * k));
  }

  /** @brief a / k; None if k == 0. */
  Option<Duration> checked_div(uint64_t k) const noexcept {
    if (k == 0) return none;
    return Some(Duration(m_nanos / k));
  }

  /* ── Saturating arithmetic ────────────────────────────────── */

  /** @brief a + b; clamped at max() on overflow. */
  Duration saturating_add(Duration o) const noexcept {
    if (m_nanos > _::k_duration_max_nanos - o.m_nanos) return Duration::max();
    return Duration(m_nanos + o.m_nanos);
  }

  /** @brief a - b; clamped at zero() if b > a. */
  Duration saturating_sub(Duration o) const noexcept {
    if (m_nanos < o.m_nanos) return Duration::zero();
    return Duration(m_nanos - o.m_nanos);
  }

  /** @brief a * k; clamped at max() on overflow. */
  Duration saturating_mul(uint64_t k) const noexcept {
    if (k == 0 || m_nanos == 0) return Duration::zero();
    if (m_nanos > _::k_duration_max_nanos / k) return Duration::max();
    return Duration(m_nanos * k);
  }

  /* ── Plain operators ───────────────────────────────────────
   *
   * Same overflow / underflow rules as Rust's debug build: panic.
   * Release builds do *not* relax this — silent wrap on a clock
   * type is the kind of bug you find at 3am in production. If you
   * want a recoverable variant, call checked*; if you want
   * clamping, call saturating*.
   */

  Duration operator+(Duration o) const noexcept {
    XPP_ASSERT(m_nanos <= _::k_duration_max_nanos - o.m_nanos, "Duration::operator+ overflow");
    return Duration(m_nanos + o.m_nanos);
  }

  Duration operator-(Duration o) const noexcept {
    XPP_ASSERT(m_nanos >= o.m_nanos, "Duration::operator- underflow (Duration is non-negative)");
    return Duration(m_nanos - o.m_nanos);
  }

  Duration operator*(uint64_t k) const noexcept {
    if (k == 0) return Duration::zero();
    XPP_ASSERT(m_nanos <= _::k_duration_max_nanos / k, "Duration::operator* overflow");
    return Duration(m_nanos * k);
  }

  Duration operator/(uint64_t k) const noexcept {
    XPP_ASSERT(k != 0, "Duration::operator/ division by zero");
    return Duration(m_nanos / k);
  }

  /**
   * @brief Ratio between two Durations (lossy).
   *
   * Mirrors Rust's Duration::div_duration_f64. Returns +infinity
   * on /zero, NaN on 0/0 — matches IEEE 754 semantics so the
   * caller sees the bad input rather than a silent panic.
   */
  double operator/(Duration o) const noexcept {
    return static_cast<double>(m_nanos) / static_cast<double>(o.m_nanos);
  }

  Duration &operator+=(Duration o) noexcept {
    *this = *this + o;
    return *this;
  }
  Duration &operator-=(Duration o) noexcept {
    *this = *this - o;
    return *this;
  }
  Duration &operator*=(uint64_t k) noexcept {
    *this = *this * k;
    return *this;
  }
  Duration &operator/=(uint64_t k) noexcept {
    *this = *this / k;
    return *this;
  }

  /* ── Comparison ────────────────────────────────────────────── */

  constexpr bool operator==(Duration o) const noexcept {
    return m_nanos == o.m_nanos;
  }
  constexpr bool operator!=(Duration o) const noexcept {
    return m_nanos != o.m_nanos;
  }
  constexpr bool operator<(Duration o) const noexcept {
    return m_nanos < o.m_nanos;
  }
  constexpr bool operator<=(Duration o) const noexcept {
    return m_nanos <= o.m_nanos;
  }
  constexpr bool operator>(Duration o) const noexcept {
    return m_nanos > o.m_nanos;
  }
  constexpr bool operator>=(Duration o) const noexcept {
    return m_nanos >= o.m_nanos;
  }

private:
  uint64_t m_nanos;
};

/** @brief k * a — symmetric multiplication. */
inline Duration operator*(uint64_t k, Duration d) noexcept {
  return d * k;
}

/* ── User-defined literals ─────────────────────────────────────────
 *
 * Opt-in via `using namespace xpp::literals;` at the TU or function
 * scope. Same pattern as std::chrono_literals so it doesn't pollute
 * every TU that includes <xpp/duration.h>.
 *
 * Each literal accepts unsigned long long (the widest integer type
 * the language hands to a numeric UDL); from_xxx then enforces the
 * uint64 range.
 *
 *   using namespace xpp::literals;
 *   mtx.try_lock_for(250_ms);
 *   sleep_for(2_s);
 */
namespace literals {

constexpr Duration operator""_ns(unsigned long long n) noexcept {
  return Duration::from_nanos(static_cast<uint64_t>(n));
}

inline Duration operator""_us(unsigned long long n) noexcept {
  return Duration::from_micros(static_cast<uint64_t>(n));
}

inline Duration operator""_ms(unsigned long long n) noexcept {
  return Duration::from_millis(static_cast<uint64_t>(n));
}

inline Duration operator""_s(unsigned long long n) noexcept {
  return Duration::from_secs(static_cast<uint64_t>(n));
}

inline Duration operator""_min(unsigned long long n) noexcept {
  return Duration::from_mins(static_cast<uint64_t>(n));
}

inline Duration operator""_h(unsigned long long n) noexcept {
  return Duration::from_hours(static_cast<uint64_t>(n));
}

} // namespace literals

} // namespace xpp

#endif // XPP_DURATION_H
