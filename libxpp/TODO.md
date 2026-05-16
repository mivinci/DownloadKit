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

Namespace: `xpp`. Headers: `<xpp/event.h>`, `<xpp/timer.h>`, etc.

## Modules (planned)

### Phase 1 — Core

| Module                  | Wraps           | Key Classes                 |
|-------------------------|-----------------|-----------------------------|
| `event.h` / `event.cpp` | `xbase/event.h` | `EventLoop`, `EventWatcher` |
| `timer.h` / `timer.cpp` | `xbase/timer.h` | `Timer`                     |
| `task.h` / `task.cpp`   | `xbase/task.h`  | `TaskGroup`, `Task`         |
| `error.h` / `error.cpp` | `xbase/error.h` | `Error` exception class     |

### Phase 2 — Network

| Module              | Wraps        | Key Classes              |
|---------------------|--------------|--------------------------|
| `dns.h` / `dns.cpp` | `xnet/dns.h` | `DnsResolver`            |
| `tcp.h` / `tcp.cpp` | `xnet/tcp.h` | `TcpConn`, `TcpListener` |
| `tls.h` / `tls.cpp` | `xnet/tls.h` | `TlsConfig`              |

### Phase 3 — HTTP

| Module                              | Wraps            | Key Classes  |
|-------------------------------------|------------------|--------------|
| `http_client.h` / `http_client.cpp` | `xhttp/client.h` | `HttpClient` |
| `http_server.h` / `http_server.cpp` | `xhttp/server.h` | `HttpServer` |
| `ws.h` / `ws.cpp`                   | `xhttp/ws.h`     | `WebSocket`  |
| `sse.h` / `sse.cpp`                 | `xhttp/sse.h`    | `SseClient`  |

### Phase 4 — Agent

| Module                      | Wraps              | Key Classes     |
|-----------------------------|--------------------|-----------------|
| `agent.h` / `agent.cpp`     | `xagent/agent.h`   | `Agent`         |
| `session.h` / `session.cpp` | `xagent/session.h` | `Session`       |
| `model.h` / `model.cpp`     | `xagent/model.h`   | `ModelRegistry` |

## Example Usage (Phase 1)

```cpp
#include <xpp/event.h>
#include <xpp/timer.h>
#include <xpp/task.h>

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
libxpp/
  TODO.md
  CMakeLists.txt
  error.h
  error.cpp
  event.h
  event.cpp
  timer.h
  timer.cpp
  task.h
  task.cpp
  ...
```

## Out of Scope

- C function/type prefix rename (`x*` → `xx*`) — separate effort
- std::future / coroutine integration — possible future addition
- Unicode / string helpers — not planned
