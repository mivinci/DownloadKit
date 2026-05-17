<!-- markdownlint-disable MD033 -->
<!-- markdownlint-disable MD041 -->

# libx++

> C++ companion to [libx](../libx/), in the spirit of Rust.

**libx++** is a small, header-mostly C++ library that complements
[libx](../libx/) — the C foundation it ships with — and reaches for
a few of Rust's safety guarantees within what plain C++11 can express.
It is not a port of `std::*`. Where the standard library would force a
copy, an exception, or an "empty after move" trap, libx++ picks the
move-only / `Result<T, E>` / niche-optimized alternative and stays
there.

The C side (libx) stays usable on its own. libx++ is opt-in: pull it
in only if you want the C++ ergonomics.

```text
xpp/
  option.h       # Option<T>             — value or none
  result.h       # Result<T, E>          — ok or err, no exceptions
  variant.h      # Variant<Ts...>        — tagged union
  error.h        # Error                 — recoverable-error value type
  nonnull.h      # NonNull<T>            — pointer that can't be null
  nonnull_own.h  # NonNullOwn<T, D>      — owning, move-only, never null
  own.h          # Own<T, D>             — nullable owning pointer
  rc.h           # Rc<T>                 — single-thread shared owning
                 # Option<Rc<T>>         — niche-optimised to sizeof(T*)
  weak.h         # Weak<T>               — non-owning observer of an Rc
  arc.h          # Arc<T> / ArcWeak<T>   — atomic, thread-safe counterparts
  panic.h        # XPP_PANIC / XPP_ASSERT — bug-trap, not error path
  handle.h       # CRTP base for opaque-handle RAII wrappers
  base/          # RAII wrappers around libx/x/base (event/timer/task)
```

## What carries over from Rust

- **Ownership is explicit.** `Own<T>` and `NonNullOwn<T>` are
  move-only (unique ownership); the wrappers in `base/` likewise.
  `Rc<T>` is shared owning (the Rust spelling, on purpose — `Ref` is
  too overloaded in C++), and its bump on copy is plain to see.
  Hot-path code can call `.clone()` or the Rust-style static
  `Rc<T>::clone(&r)` to make "+1 on the count" loud at the call
  site; the implicit copy ctor remains available for the common
  case. There is no implicit deep-copy of T.
- **Single-thread vs cross-thread, explicit.** `Rc<T>` is
  single-thread; sharing one across threads is UB. `Arc<T>` (in
  `xpp/arc.h`) is the thread-safe counterpart with the same shape
  and atomic strong/weak counts under the hood — same trade-off
  Rust draws between `std::rc::Rc` and `std::sync::Arc`. Pick the
  cheap one when you can, pay for atomic only when you must.
- **Weak references break cycles.** Build trees / DAGs with `Rc<T>`
  on forward edges and `Weak<T>` on back-edges — exactly Rust's
  `Rc<T>` / `Weak<T>` idiom. `Weak::upgrade()` returns
  `Option<Rc<T>>`: `Some` if at least one strong is still around,
  `None` otherwise. `Arc<T>` has its matching `ArcWeak<T>` with a
  CAS-loop upgrade for cross-thread soundness.
- **Type-level non-null.** `NonNull<T>` and `NonNullOwn<T>` make
  "this pointer is not null" a compile-time fact. The same idea
  applies to `Rc<T>` / `Arc<T>`: each always points at a live T
  (strong ≥ 1), and nullability is opted into by wrapping it in
  `Option<Rc<T>>` / `Option<Arc<T>>`. `Option<NonNull<T>>`,
  `Option<NonNullOwn<T>>`, `Option<Rc<T>>`, and `Option<Arc<T>>`
  are all **niche-optimised** to `sizeof(T*)` — matching Rust's
  `Option<&T>`, `Option<Box<T>>`, `Option<Rc<T>>`, and
  `Option<Arc<T>>` respectively. `Weak<T>` and `ArcWeak<T>` are
  already nullable by definition (default-constructed = null), so
  no Option wrapping is needed — same as Rust. Verified by
  `static_assert` in each header.
- **Errors are values.** `Result<T, E>` is the recoverable-error
  channel — no exceptions, no `errno`. Combinators read like Rust:
  `map`, `mapErr`, `andThen`, `orElse`, `inspect`, `transpose`,
  `unwrap`, `unwrapOr`, `unwrapErr`, `okOr`, `take`. Pattern-matching
  on `Variant<Ts...>` is the closest C++11 gets to `match`.
- **Panic vs error, separated.** `XPP_PANIC` / `XPP_ASSERT` are for
  bugs (precondition violations, "this can't happen"). They route
  through `xbase/log`'s fatal channel and abort. `Result<T, E>` is for
  things callers are expected to handle. Don't blur the two.
- **Zero overhead.** No virtual dispatch, no extra allocation on the
  hot path. EBO on stateless deleters; `sizeof(Own<T>) == sizeof(T*)`
  and `sizeof(NonNullOwn<T>) == sizeof(T*)` for the default deleter.
  `sizeof(Rc<T>) == sizeof(Arc<T>) == sizeof(Weak<T>) ==
  sizeof(ArcWeak<T>) == sizeof(T*)` too — single heap allocation per
  `makeRc`/`makeArc`, control block (strong + weak counts) co-located
  with T, no separate control block like `std::shared_ptr`.

## What doesn't translate

These are the corners where C++ pulls back the rug, named so you don't
expect them:

- **No borrow checker.** `NonNull<T>` says "the pointer is not null at
  this point", not "no one else can mutate the pointee while you hold
  this". Aliasing, lifetimes, and use-after-free remain your problem.
- **Moved-from objects still exist.** Move construction leaves a
  defined-but-unspecified state behind, just like `std::unique_ptr`.
  `NonNullOwn`'s, `Rc`'s, and `Arc`'s "moved-from" state holds a null
  internally — visible to the destructor only; using `get() /
  operator* / operator->` on a moved-from value is UB. Same contract
  as the standard library.
- **No native pattern matching.** `Variant<Ts...>` exists, but you
  visit it with functor structs or hand-written `if (v.is<T>())`
  chains. C++11 has no `match` and no generic lambdas (the latter
  arrived in C++14, which we don't require — see below).
- **No traits / typeclasses.** Customization is by overload, by
  template specialization, or by deleter type. SFINAE where you must;
  CRTP where it's clean.

## C++11

libx++ targets **strict C++11**. The CI compile guard
(`x++_cxx11_guard`, opt in with `-DXPP_CXX11_GUARD=ON`) compiles every
public header under `-std=c++11` with no GNU extensions, instantiating
the templates. New code must stay 11-clean — no generic lambdas, no
`auto` deduced returns, no `std::make_unique`, no `std::is_final`
without a fallback.

The reason is portability of the wider libx family: libx is C99,
libx++ at C++11 is the lowest C++ standard that still has full move
semantics and templates, so a downstream consumer can pull libx++ into
a C++11-only codebase without forcing a global standard bump. If a
type alias from C++14 (`enable_if_t`, `decay_t`, …) genuinely helps,
add a `_::Foo` shim with a 14+/intrinsic/fallback chain.

## Naming

| C API (`libx`)    | C++ API (`libx++`)      |
|-------------------|-------------------------|
| `xEventLoop`      | `xpp::EventLoop`        |
| `xEventLoopRun()` | `xpp::EventLoop::run()` |
| `xTimer`          | `xpp::Timer`            |
| `xTask`           | `xpp::Task`             |
| `xErrno_Ok`       | (no return / no throw)  |
| `xErrno_Busy`     | `Result<T, xErrno>`     |

Namespace is `xpp`. Headers are scoped:

```cpp
#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/own.h>

#include <xpp/base/event.h>   // wrappers over libx/x/base/event.h
#include <xpp/base/timer.h>
#include <xpp/base/task.h>
```

The submodule layout under `xpp/` mirrors `libx/x/<module>/`. Phase 2
will add `xpp/net/`, Phase 3 `xpp/http/`, Phase 4 `xpp/agent/`. See
[`xpp/TODO.md`](xpp/TODO.md) for the roadmap.

## Build

libx++ ships as part of moo's CMake tree:

```bash
cmake -S . -B build
cmake --build build --target x++         # the library
cmake --build build --target x++_test    # GoogleTest unit tests
```

Standalone consumption: libx++ depends on libx (the `x++` target
PUBLIC-links `xbase`), so a downstream CMake project pulls in both:

```cmake
add_subdirectory(third_party/moo/libx)     # provides xbase, etc.
add_subdirectory(third_party/moo/libx++)
target_link_libraries(my_app PRIVATE x++)  # alias: xpp
```

The `x++` target carries `cxx_std_11` as a public feature, so any
target linking against it inherits C++11 minimum.

## Status

Phase 1 (`base/`) is the working surface today —
`EventLoop` / `Timer` / `Task` plus the value types
(`Option` / `Result` / `Variant` / `NonNull` / `Own`). Phases 2-4 are
sketched in [`xpp/TODO.md`](xpp/TODO.md) but not yet implemented.

API is stable enough to use; signatures may still shift before a 1.0.
