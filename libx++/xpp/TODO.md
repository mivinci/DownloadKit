# libxpp — C++ RAII Wrappers for libx

Thin C++ wrappers that RAII-ify the C API in `libx/`.

## Principles

- **Zero overhead** — no virtual dispatch, no extra allocation on hot
  paths.
- **RAII-first** — constructors acquire, destructors release. No
  `x*Create`/`x*Destroy` pair leaks.
- **Throw on error** — C functions returning `xErrno` become C++
  functions that throw `xpp::Error` (a thin wrapper around `xErrno`).
  Callers who prefer error codes can use the `xpp::no_throw` adaptor or
  call the C API directly.
- **Move-only** — most libx handles are non-copyable. Wrapped types are
  move-only by default.

## Naming Convention

| C API             | C++ API                 |
|-------------------|-------------------------|
| `xEventLoop`      | `xpp::EventLoop`        |
| `xEventLoopRun()` | `xpp::EventLoop::run()` |
| `xTimer`          | `xpp::Timer`            |
| `xTask`           | `xpp::Task`             |
| `xErrno_Ok`       | no throw                |
| `xErrno_Busy`     | `xpp::Error{Busy}`      |

Namespace: `xpp`. Headers: `<xpp/base/event.h>`, `<xpp/base/timer.h>`,
etc. — submodule layout mirrors `libx/x/<module>/`.

## Modules (planned)

### Phase 1 — Core

| Module                            | Wraps             | Key Classes                 |
|-----------------------------------|-------------------|-----------------------------|
| `base/event.h` / `base/event.cpp` | `x/base/event.h`  | `EventLoop`, `EventWatcher` |
| `base/timer.h` / `base/timer.cpp` | `x/base/timer.h`  | `Timer`                     |
| `base/task.h`  / `base/task.cpp`  | `x/base/task.h`   | `TaskGroup`, `Task`         |
| `base/error.h` / `base/error.cpp` | `x/base/error.h`  | `Error` exception class     |

### Phase 2 — Network

| Module                        | Wraps          | Key Classes              |
|-------------------------------|----------------|--------------------------|
| `net/dns.h` / `net/dns.cpp`   | `x/net/dns.h`  | `DnsResolver`            |
| `net/tcp.h` / `net/tcp.cpp`   | `x/net/tcp.h`  | `TcpConn`, `TcpListener` |
| `net/tls.h` / `net/tls.cpp`   | `x/net/tls.h`  | `TlsConfig`              |

### Phase 3 — HTTP

| Module                                        | Wraps             | Key Classes  |
|-----------------------------------------------|-------------------|--------------|
| `http/http_client.h` / `http/http_client.cpp` | `x/http/client.h` | `HttpClient` |
| `http/http_server.h` / `http/http_server.cpp` | `x/http/server.h` | `HttpServer` |
| `http/ws.h`          / `http/ws.cpp`          | `x/http/ws.h`     | `WebSocket`  |
| `http/sse.h`         / `http/sse.cpp`         | `x/http/sse.h`    | `SseClient`  |

### Phase 4 — Agent

| Module                                  | Wraps                | Key Classes     |
|-----------------------------------------|----------------------|-----------------|
| `agent/agent.h`   / `agent/agent.cpp`   | `x/agent/agent.h`    | `Agent`         |
| `agent/session.h` / `agent/session.cpp` | `x/agent/session.h`  | `Session`       |
| `agent/model.h`   / `agent/model.cpp`   | `x/agent/model.h`    | `ModelRegistry` |

## Example Usage (Phase 1)

```cpp
#include <xpp/base/event.h>
#include <xpp/base/timer.h>
#include <xpp/base/task.h>

int main() {
  xpp::EventLoop loop;

  // One-shot timer, auto-disarmed on destruction
  xpp::Timer t(loop, 1000, [] { /* on expiry */ });
  t.arm();

  // Task group with RAII cleanup
  xpp::TaskGroup tg(4);  // 4 threads
  auto task = tg.submit(
    [](void*) -> void* { /* work */ return nullptr; }, nullptr);
  // task.wait(); // optional — destructor also waits

  loop.run();
  return 0;
}
```

## Error Handling

```cpp
// Default: throws xpp::Error on failure
xpp::EventLoop loop;  // throws if xEventLoopCreate returns NULL

// Non-throwing variant for interop
auto loop = xpp::EventLoop::create_no_throw();
if (!loop) { /* handle xErrno */ }
```

## Build Integration

CMake target `xpp` (compiled shared/static library):

```cmake
add_library(xpp
  error.cpp
  event.cpp
  timer.cpp
  task.cpp
)
target_link_libraries(xpp PUBLIC xbase)
```

Consumers:

```cmake
target_link_libraries(my_app PRIVATE xpp)
```

## Directory Layout

```text
libx++/xpp/
  TODO.md
  CMakeLists.txt
  compiler.h            # portable attribute / intrinsic macros
  handle.h              # raw-handle CRTP base
  in_place.h            # in-place construction tag types
  panic.h
  option.h              # value-only Option<T>
  result.h              # value-or-error Result<T, E>
  variant.h             # tagged union
  nonnull.h             # NonNull<T>
  nonnull_own.h         # NonNullOwn<T, Deleter>
  own.h                 # Own<T> aka unique_ptr-with-niches
  cxx11_guard.cpp       # strict-mode compile guard (off by default)
  base/                 # wrappers over libx/x/base
    event.h / event.cpp
    timer.h / timer.cpp
    task.h  / task.cpp
    error.h / error.cpp # planned
  net/                  # Phase 2 — wrappers over libx/x/net
  http/                 # Phase 3 — wrappers over libx/x/http
  agent/                # Phase 4 — wrappers over libx/x/agent
```

## Out of Scope

- C function/type prefix rename (`x*` → `xx*`) — separate effort
- std::future / coroutine integration — possible future addition
- Unicode / string helpers — not planned
