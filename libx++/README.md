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
  ref.h          # Ref<T>                — shared owning (Rust-style Rc)
                 # Option<Ref<T>>        — niche-optimised to sizeof(T*)
  panic.h        # XPP_PANIC / XPP_ASSERT — bug-trap, not error path
  handle.h       # CRTP base for opaque-handle RAII wrappers
  base/          # RAII wrappers around libx/x/base (event/timer/task)
```

## What carries over from Rust

- **Ownership is explicit.** `Own<T>` and `NonNullOwn<T>` are
  move-only (unique ownership); the wrappers in `base/` likewise.
  `Ref<T>` is shared owning, but its bump on copy is plain to see —
  there's a `.clone()` alias for hot-path code that wants to call out
  "+1 on the count" loudly. The standard library default — copy
  silently does a `shared_ptr` bump — works the same here, just with
  half the size. There is no implicit deep-copy of T.
- **Type-level non-null.** `NonNull<T>` and `NonNullOwn<T>` make
  "this pointer is not null" a compile-time fact. The same idea
  applies to `Ref<T>`: a `Ref<T>` always points at a live T (its
  count is ≥ 1), and nullability is opted into by wrapping it in
  `Option<Ref<T>>`. `Option<NonNull<T>>`, `Option<NonNullOwn<T>>`,
  and `Option<Ref<T>>` are all **niche-optimised** to `sizeof(T*)` —
  matching Rust's `Option<&T>`, `Option<Box<T>>`, and `Option<Rc<T>>`
  respectively. Verified by `static_assert` in the headers.
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
  `sizeof(Ref<T>) == sizeof(T*)` too — single heap allocation per
  `makeRef`, control block (just a strong count) co-located with T,
  no separate control block like `std::shared_ptr`.

## What doesn't translate

These are the corners where C++ pulls back the rug, named so you don't
expect them:

- **No borrow checker.** `NonNull<T>` says "the pointer is not null at
  this point", not "no one else can mutate the pointee while you hold
  this". Aliasing, lifetimes, and use-after-free remain your problem.
- **Moved-from objects still exist.** Move construction leaves a
  defined-but-unspecified state behind, just like `std::unique_ptr`.
  `NonNullOwn`'s and `Ref`'s "moved-from" state holds a null
  internally — visible to the destructor only; using `get() /
  operator* / operator->` on a moved-from value is UB. Same contract
  as the standard library.
- **No `Weak<T>` yet.** `Ref<T>` is strong-only. A reference cycle
  built out of `Ref`s leaks — there's no garbage collector and no
  weak escape hatch. For tree / DAG ownership this is fine; for
  graphs with back-edges, store back-edges as raw pointers (the
  forward `Ref` chain keeps everything alive) or wait for `Weak<T>`
  to land.
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
