/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - xpp::io::Error: a bit-packed I/O error type.
 *
 * Modeled after Rust's std::io::Error.  An Error is one of:
 *   - SimpleMessage  — static (kind, message) pair, zero-allocation
 *   - Custom         — kind + heap-allocated message
 *   - Os             — raw errno + kind derived via errno_to_kind()
 *   - Simple         — bare ErrorKind, no message
 *
 * All four variants are packed into a single uintptr_t (64-bit only),
 * with the two low bits used as a tag.  sizeof(Error) == sizeof(void*)
 * and sizeof(Option<Error>) == sizeof(void*) via niche optimisation.
 *
 * Move-only.  The Custom variant owns a heap allocation that the
 * destructor frees.
 *
 * C++11-compatible.
 */

#ifndef XPP_IO_ERROR_H
#define XPP_IO_ERROR_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/string.h>

#include <cstddef>
#include <cstdint>

namespace xpp {

namespace io {

/* ── ErrorKind ──────────────────────────────────────────────────────── */

/**
 * @brief Cross-platform error category, mirroring std::io::ErrorKind.
 *
 * Numeric value 0 is reserved as a niche for Option<Error>.  Do not
 * use _Niche directly; the static_assert at the bottom of this header
 * enforces the invariant.
 */
enum class ErrorKind : uint8_t {
  _Niche             = 0, // reserved — Option<Error>'s None pattern
  NotFound           = 1,
  PermissionDenied,
  ConnectionRefused,
  ConnectionReset,
  ConnectionAborted,
  NotConnected,
  AddrInUse,
  AddrNotAvailable,
  NetworkDown,
  NetworkUnreachable,
  HostUnreachable,
  BrokenPipe,
  AlreadyExists,
  WouldBlock,
  InvalidInput,
  InvalidData,
  TimedOut,
  WriteZero,
  Interrupted,
  Unsupported,
  UnexpectedEof,
  OutOfMemory,
  ResourceBusy,
  ResolveFailed, // DNS resolution failed
  NoAddress,     // host resolved to zero usable addresses
  Other,
};

/// Human-readable name for an ErrorKind ("ConnectionRefused", "Other", ...).
const char *kind_name(ErrorKind k) noexcept;

/// Map a POSIX errno value to an ErrorKind.  Unknown codes → Other.
ErrorKind errno_to_kind(int errno_value) noexcept;

/* ── SimpleMessage ──────────────────────────────────────────────────── */

/**
 * @brief Static (kind, msg) pair used by the SimpleMessage variant.
 *
 * Has natural pointer alignment (8 bytes on 64-bit) so the two low
 * bits of its address are always zero — they're available as tag
 * bits without further annotation.  Use XPP_IO_DEFINE_MSG to declare
 * instances.
 */
struct SimpleMessage {
  ErrorKind   kind;
  const char *msg;
};

/**
 * @brief Define a function returning a stable const SimpleMessage*.
 *
 * Magic-statics give us ODR-safe singletons in C++11; one definition
 * per process per .so.  Use the function form so the address is taken
 * lazily — important because static SimpleMessage initialization
 * happens at first call, not before main().
 *
 * Usage:
 *   XPP_IO_DEFINE_MSG(closed_socket, NotConnected, "operation on closed socket")
 *   ...
 *   return io::Error::from_static(closed_socket());
 *
 * Cross-`.so` callers may receive distinct addresses for the same
 * message; compare by kind(), never by pointer identity.
 */
#define XPP_IO_DEFINE_MSG(NAME, KIND, MSG)                                                         \
  inline const ::xpp::io::SimpleMessage *NAME() noexcept {                                         \
    static const ::xpp::io::SimpleMessage v{::xpp::io::ErrorKind::KIND, MSG};                      \
    return &v;                                                                                    \
  }

/* ── Repr details (private) ─────────────────────────────────────────── */

namespace _ {

/**
 * @brief Heap-allocated payload for the Custom variant.
 *
 * Owns its message inline via a flexible-array-style trailer, so a
 * Custom error costs exactly one allocation.  Naturally ≥4-byte
 * aligned (uint32_t member) — the tag-bit invariant holds.
 */
struct Custom {
  ErrorKind kind;
  uint32_t  len;
  char      data[1]; // trailer; sized via custom allocator

  // No automatic ctor/dtor — managed by custom_alloc / custom_free.
};

/**
 * @brief Allocate and populate a Custom payload.
 *
 * Allocates `sizeof(Custom) - 1 + len + 1` bytes (one trailing NUL),
 * copies @p msg into the trailer, returns the pointer.  Aborts on
 * allocation failure (via panic) — matches Box's semantics.
 */
Custom *custom_alloc(ErrorKind kind, const char *msg, size_t len);

/// Free a Custom payload allocated by custom_alloc.
void custom_free(Custom *c) noexcept;

} // namespace _

/* ── Error ──────────────────────────────────────────────────────────── */

/**
 * @brief A bit-packed I/O error.  See file header comment for layout.
 */
class Error {
public:
  using Kind = ErrorKind;

  /** @brief Construct from a bare ErrorKind (Simple variant).  Implicit. */
  Error(ErrorKind kind) noexcept : bits_(pack_simple(kind)) {
    XPP_ASSERT(kind != ErrorKind::_Niche, "io::Error: ErrorKind::_Niche is reserved");
  }

  /** @brief Capture errno as an Os variant.  No allocation. */
  static Error from_errno(int errno_value) noexcept {
    Error e(_PrivateTag{});
    e.bits_ = pack_os(errno_value);
    return e;
  }

  /** @brief Wrap a static SimpleMessage.  No allocation. */
  static Error from_static(const SimpleMessage *msg) noexcept {
    XPP_ASSERT(msg != nullptr, "io::Error::from_static: null SimpleMessage");
    Error e(_PrivateTag{});
    e.bits_ = pack_simple_message(msg);
    return e;
  }

  /** @brief Build a Custom variant by copying @p msg into the heap. */
  static Error with_message(ErrorKind kind, const char *msg, size_t len) {
    XPP_ASSERT(kind != ErrorKind::_Niche, "io::Error: ErrorKind::_Niche is reserved");
    _::Custom *c = _::custom_alloc(kind, msg, len);
    Error      e(_PrivateTag{});
    e.bits_ = pack_custom(c);
    return e;
  }
  static Error with_message(ErrorKind kind, const String &msg) {
    return with_message(kind, msg.c_str(), msg.len());
  }
  static Error with_message(ErrorKind kind, const char *msg) {
    size_t len = 0;
    if (msg) {
      while (msg[len] != '\0') ++len;
    }
    return with_message(kind, msg, len);
  }

  /* ── Move semantics ───────────────────────────────────────────────── */

  Error(Error &&o) noexcept : bits_(o.bits_) {
    o.bits_ = pack_simple(ErrorKind::Other); // moved-from = harmless Simple
  }
  Error &operator=(Error &&o) noexcept {
    if (this != &o) {
      drop();
      bits_   = o.bits_;
      o.bits_ = pack_simple(ErrorKind::Other);
    }
    return *this;
  }
  Error(const Error &)            = delete;
  Error &operator=(const Error &) = delete;
  ~Error() {
    drop();
  }

  /* ── Accessors ────────────────────────────────────────────────────── */

  /** @brief The error category. */
  ErrorKind kind() const noexcept {
    switch (tag()) {
    case kTagSimpleMessage:
      return as_simple_message()->kind;
    case kTagCustom:
      return as_custom()->kind;
    case kTagOs:
      return errno_to_kind(static_cast<int>(static_cast<int32_t>(bits_ >> 32)));
    case kTagSimple:
    default:
      return static_cast<ErrorKind>(static_cast<uint8_t>(bits_ >> 8));
    }
  }

  /** @brief Underlying errno if this is an Os error, else None. */
  Option<int> raw_os_error() const noexcept {
    if (tag() != kTagOs) return Option<int>(none);
    int v = static_cast<int>(static_cast<int32_t>(bits_ >> 32));
    return Option<int>(v);
  }

  /**
   * @brief Borrow the message string when one exists.
   *
   * Returns a pointer to the message for SimpleMessage and Custom
   * variants (NUL-terminated, valid for the lifetime of the Error).
   * Returns nullptr for Simple and Os.
   */
  const char *message() const noexcept {
    switch (tag()) {
    case kTagSimpleMessage:
      return as_simple_message()->msg;
    case kTagCustom:
      return as_custom()->data;
    default:
      return nullptr;
    }
  }

  /// Format as "Kind: message" / "Kind (os error N)" / "Kind".
  String to_string() const;

  /* ── Niche access (used by Option<Error> specialization below) ──── */

  /// Internal — exposes the packed representation for the Option niche.
  uintptr_t bits_for_niche() const noexcept {
    return bits_;
  }

  /// Internal — bit pattern of a harmless Simple(Other) error.  Used
  /// by Option<Error> to neutralise the moved-from source after
  /// stealing its bits, without running ~Error on it.
  static uintptr_t pack_simple_for_niche() noexcept {
    return pack_simple(ErrorKind::Other);
  }

private:
  /* ── Tag scheme (low 2 bits of bits_) ──────────────────────────── */
  static constexpr uintptr_t kTagMask         = 0x3u;
  static constexpr uintptr_t kTagSimpleMessage = 0x0u;
  static constexpr uintptr_t kTagCustom        = 0x1u;
  static constexpr uintptr_t kTagOs            = 0x2u;
  static constexpr uintptr_t kTagSimple        = 0x3u;

  struct _PrivateTag {};
  Error(_PrivateTag) noexcept : bits_(0) {}

  uintptr_t tag() const noexcept {
    return bits_ & kTagMask;
  }

  /* ── Pack/unpack helpers ──────────────────────────────────────── */

  static uintptr_t pack_simple(ErrorKind k) noexcept {
    // Low 2 bits = kTagSimple (0b11); kind in bits 8..15.
    return (static_cast<uintptr_t>(static_cast<uint8_t>(k)) << 8) | kTagSimple;
  }
  static uintptr_t pack_os(int32_t errno_value) noexcept {
    // Low 2 bits = kTagOs; errno in bits 32..63 (sign-extended).
    return (static_cast<uintptr_t>(static_cast<uint32_t>(errno_value)) << 32) | kTagOs;
  }
  static uintptr_t pack_simple_message(const SimpleMessage *p) noexcept {
    return reinterpret_cast<uintptr_t>(p) | kTagSimpleMessage;
  }
  static uintptr_t pack_custom(_::Custom *p) noexcept {
    return reinterpret_cast<uintptr_t>(p) | kTagCustom;
  }

  const SimpleMessage *as_simple_message() const noexcept {
    return reinterpret_cast<const SimpleMessage *>(bits_ & ~kTagMask);
  }
  _::Custom *as_custom() const noexcept {
    return reinterpret_cast<_::Custom *>(bits_ & ~kTagMask);
  }

  /* ── Drop ─────────────────────────────────────────────────────── */

  void drop() noexcept {
    if (tag() == kTagCustom) {
      _::custom_free(as_custom());
    }
    // SimpleMessage / Os / Simple own no heap state.
  }

  /* ── Storage ──────────────────────────────────────────────────── */

  uintptr_t bits_;

  // Allow the niche specialization to construct an instance directly.
  template <class T> friend class ::xpp::Option;
};

/* ── Compile-time invariants ───────────────────────────────────────── */

static_assert(sizeof(void *) == 8, "xpp::io::Error requires a 64-bit target");
static_assert(sizeof(Error) == sizeof(void *), "xpp::io::Error must be one pointer wide");
static_assert(static_cast<int>(ErrorKind::_Niche) == 0,
              "ErrorKind::_Niche must be 0 for the Option niche");
static_assert(static_cast<int>(ErrorKind::NotFound) != 0,
              "ErrorKind::NotFound must not collide with the niche");

} // namespace io

/* ── Option<io::Error> niche specialization ─────────────────────────── */

/**
 * @brief Option<io::Error> stays one pointer wide — bits_ == 0 is the
 *        None pattern, which no valid Error can produce (every tag
 *        either points to an aligned object or carries a non-zero
 *        payload + non-zero tag bits).
 */
template <> class Option<io::Error> {
public:
  using value_type = io::Error;

  Option() noexcept : bits_(0) {}
  Option(None) noexcept : bits_(0) {}
  Option(io::Error &&e) noexcept : bits_(e.bits_for_niche()) {
    // Steal e's bits.  We must avoid running ~Error on the source —
    // that would free the Custom payload we just absorbed.  Instead
    // overwrite e's storage with a Simple(Other) bit pattern, which
    // owns no heap state.  e's destructor will see kTagSimple and
    // do nothing.
    *reinterpret_cast<uintptr_t *>(&e) = io::Error::pack_simple_for_niche();
  }

  Option(const Option &)            = delete;
  Option &operator=(const Option &) = delete;

  Option(Option &&o) noexcept : bits_(o.bits_) {
    o.bits_ = 0;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      clear();
      bits_   = o.bits_;
      o.bits_ = 0;
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    clear();
    return *this;
  }

  ~Option() {
    clear();
  }

  bool is_some() const noexcept {
    return bits_ != 0;
  }
  bool is_none() const noexcept {
    return bits_ == 0;
  }
  explicit operator bool() const noexcept {
    return is_some();
  }

  io::Error unwrap() && {
    XPP_ASSERT(is_some(), "Option<io::Error>::unwrap on None");
    // Construct a fresh Error around our bits_ and clear ours.
    io::Error out(io::ErrorKind::Other); // placeholder; bit pattern overwritten below
    // Direct bit overwrite is the only way without exposing more friends.
    // The placeholder Error has tag=Simple so no heap state is leaked.
    *reinterpret_cast<uintptr_t *>(&out) = bits_;
    bits_                                = 0;
    return out;
  }

  /// Inspect the kind without consuming.
  io::ErrorKind kind() const {
    XPP_ASSERT(is_some(), "Option<io::Error>::kind on None");
    // Re-derive without forming a real Error: read tag from bits_.
    const uintptr_t kTagMask = 0x3u;
    const uintptr_t t        = bits_ & kTagMask;
    if (t == 0x3u) { // Simple
      return static_cast<io::ErrorKind>(static_cast<uint8_t>(bits_ >> 8));
    }
    // Re-use Error's kind() for the other variants.  This is safe
    // because Error's storage is exactly bits_ and its kind() is a
    // read-only operation.
    const io::Error *as_error = reinterpret_cast<const io::Error *>(this);
    return as_error->kind();
  }

private:
  void clear() noexcept {
    if (bits_ == 0) return;
    // Destroy via Error's destructor — its bits layout matches ours.
    io::Error *as_error = reinterpret_cast<io::Error *>(this);
    as_error->~Error();
    bits_ = 0;
  }

  uintptr_t bits_;
};

static_assert(sizeof(Option<io::Error>) == sizeof(void *),
              "Option<io::Error> niche broken");

} // namespace xpp

#endif // XPP_IO_ERROR_H
