<!-- markdownlint-disable MD033 -->
<!-- markdownlint-disable MD041 -->

# libx++

> Rust-like infrastructure in C++11.

**libx++** is a small, header-mostly C++ library that reaches for a
few of Rust's safety guarantees within what plain C++11 can express.
It is not a port of `std::*`. Where the standard library would force
a copy, an exception, or an "empty after move" trap, libx++ picks the
move-only / `Result<T, E>` / niche-optimised alternative and stays
there.

The current surface is the value-type + RAII + ref-counted core:
`Option`, `Result`, `Variant`, `Error`, `NonNull`, `NonNullOwn`,
`Own`, `Rc`, `Weak`, `Arc`, `ArcWeak`, plus the panic and CRTP-handle
helpers. Domain-specific bindings (e.g. C-API wrappers) are
deliberately out of scope for libx++ itself — build them as separate
projects that depend on libx++ when you need them.

```text
xpp/
  option.h       # Option<T>             — value or none
  result.h       # Result<T, E>          — ok or err, no exceptions
  variant.h      # Variant<Ts...>        — tagged union
  error.h        # Error                 — int-like recoverable-error value
  nonnull.h      # NonNull<T>            — pointer that can't be null
  nonnull_own.h  # NonNullOwn<T, D>      — owning, move-only, never null
  own.h          # Own<T, D>             — nullable owning pointer
  rc.h           # Rc<T>                 — single-thread shared owning
                 # Option<Rc<T>>         — niche-optimised to sizeof(T*)
  weak.h         # Weak<T>               — non-owning observer of an Rc
  arc.h          # Arc<T> / ArcWeak<T>   — atomic, thread-safe counterparts
  mutex.h        # Mutex<T> / MutexGuard<T> — data + lock fusion
  cond.h         # Condvar                  — condition variable companion
  panic.h        # XPP_PANIC / XPP_ASSERT — bug-trap, not error path
  handle.h       # CRTP base for opaque-handle RAII wrappers
  compiler.h     # portable attribute / intrinsic macros
  in_place.h     # in-place construction tag types
```

## What carries over from Rust

- **Ownership is explicit.** `Own<T>` and `NonNullOwn<T>` are
  move-only (unique ownership). `Rc<T>` is shared owning (the Rust
  spelling, on purpose — `Ref` is too overloaded in C++), and its
  bump on copy is plain to see. Hot-path code can call `.clone()`
  or the Rust-style static `Rc<T>::clone(&r)` to make "+1 on the
  count" loud at the call site; the implicit copy ctor remains
  available for the common case. There is no implicit deep-copy
  of T.
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
  CAS-loop upgrade for cross-thread soundness. Worked examples in
  the header docstrings of `xpp/weak.h` (parent ⇄ child tree) and
  `xpp/arc.h` (thread-safe publisher / subscriber).
- **Data + lock fusion.** `Mutex<T>` (in `xpp/mutex.h`) wraps the T
  it protects with the lock that protects it; the only way to
  touch the data is through a `MutexGuard<T>` returned by `lock()`
  / `try_lock()`. The compiler-enforced "you can't reach the value
  without taking the lock" matches Rust's `std::sync::Mutex<T>`.
  `Condvar` (in `xpp/cond.h`) is the companion condition
  variable, split into its own header the way Rust splits
  `std::sync::Mutex` and `std::sync::Condvar`. `wait(guard)`
  atomically releases the lock + sleeps + re-acquires, matching
  Rust's `Condvar::wait`.
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
  `map`, `map_err`, `and_then`, `or_else`, `inspect`, `transpose`,
  `unwrap`, `unwrap_or`, `unwrap_err`, `ok_or`, `take`. Pattern-matching
  on `Variant<Ts...>` is the closest C++11 gets to `match`. `Error`
  in `xpp/error.h` is the canonical error type — a thin newtype
  over an int-like code that lets you bridge any foreign-API
  errno / status into `Result<T, Error>` at one point and inspect
  with `.code()` on the error path.
- **Panic vs error, separated.** `XPP_PANIC` / `XPP_ASSERT` are for
  bugs (precondition violations, "this can't happen"). They route
  through `xbase/log`'s fatal channel and abort. `Result<T, E>` is
  for things callers are expected to handle. Don't blur the two.
- **Zero overhead.** No virtual dispatch, no extra allocation on the
  hot path. EBO on stateless deleters; `sizeof(Own<T>) == sizeof(T*)`
  and `sizeof(NonNullOwn<T>) == sizeof(T*)` for the default deleter.
  `sizeof(Rc<T>) == sizeof(Arc<T>) == sizeof(Weak<T>) ==
  sizeof(ArcWeak<T>) == sizeof(T*)` too — single heap allocation per
  `make_rc`/`make_arc`, control block (strong + weak counts) co-located
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

The reason is downstream reach: C++11 is the lowest C++ standard that
still has full move semantics + templates, so a consumer can pull
libx++ into a C++11-only codebase without forcing a global standard
bump. If a type alias from C++14 (`enable_if_t`, `decay_t`, …)
genuinely helps, add a `_::Foo` shim with a 14+/intrinsic/fallback
chain in the same vein as `_::IsFinal` in `nonnull_own.h`.

## Naming

Namespace is `xpp`. Headers are scoped:

```cpp
#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/own.h>
#include <xpp/rc.h>
#include <xpp/weak.h>
#include <xpp/arc.h>
```

Method names lean Rust:

| Operation              | libx++                              | Rust                            |
|------------------------|-------------------------------------|---------------------------------|
| Construct in place     | `make_rc<T>(args...)`                | `Rc::new(T::new(...))`          |
| Explicit +1            | `r.clone()` / `Rc<T>::clone(&r)`    | `r.clone()` / `Rc::clone(&r)`   |
| Strong count           | `r.strong_count()`                   | `Rc::strong_count(&r)`          |
| Weak count             | `r.weak_count()`                     | `Rc::weak_count(&r)`            |
| Downgrade to Weak      | `Rc<T>::downgrade(r)`               | `Rc::downgrade(&r)`             |
| Upgrade from Weak      | `w.upgrade()` → `Option<Rc<T>>`     | `w.upgrade()` → `Option<Rc<T>>` |
| Result combinators     | `.map / .map_err / .and_then / .or_else / .unwrap / .unwrap_or` | identical |

Where the C++ standard library has a strongly-settled spelling
(`r.get()` for the raw pointer, copy ctor for the common
"borrow-or-clone" case) libx++ keeps it rather than mechanically
overriding to Rust — the goal is "Rust-like", not "indistinguishable
from Rust".

## Build

libx++ ships as part of moo's CMake tree:

```bash
cmake -S . -B build
cmake --build build --target x++_test    # GoogleTest unit tests
```

The library target itself (`x++`) is a tiny static archive built
from a single TU (`panic.cpp`), which implements the panic-message
routing. Every other primitive is header-only and instantiates in
the consumer's TU. `x++` also carries the include path, the
`cxx_std_11` feature requirement, and a transitive link to `xbase`
(because `panic.cpp` dispatches through `xLog`). Anyone depending
on `x++` gets all three.

Standalone consumption — libx++ currently depends on libx through
the panic-to-xLog implementation, so a downstream CMake project
pulls in both:

```cmake
add_subdirectory(third_party/moo/libx)     # provides xbase, etc.
add_subdirectory(third_party/moo/libx++)
target_link_libraries(my_app PRIVATE x++)  # alias: xpp
```

## Status

The Rust-flavoured primitives are working surface: `Option`,
`Result`, `Variant`, `Error`, `NonNull`, `NonNullOwn`, `Own`, `Rc`,
`Weak`, `Arc`, `ArcWeak`, plus the panic / handle / compiler /
in-place helpers. Each ships with a focused GoogleTest suite; the
whole collection is ~300 tests.

API is stable enough to use; signatures may still shift before a 1.0.
