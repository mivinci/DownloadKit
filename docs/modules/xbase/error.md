# error.h — Unified Error Codes

## Introduction

`error.h` defines a unified set of error codes (`xErrno`) used throughout xKit. Every function that can fail returns an `xErrno` value, providing a consistent error handling pattern across all modules. The companion function `xstrerror()` converts error codes to human-readable strings for logging and debugging.

## Design Philosophy

1. **Single Error Enum** — All xKit modules share one error code enum, avoiding the confusion of module-specific error types. This makes error handling uniform: check for `xErrno_Ok` everywhere.

2. **Descriptive Codes** — Each error code maps to a specific failure category (invalid argument, out of memory, wrong state, etc.), giving callers enough information to decide how to handle the error without inspecting errno or platform-specific codes.

3. **Human-Readable Messages** — `xstrerror()` returns a static string for each code, suitable for direct inclusion in log messages. It never returns NULL.

## Architecture

```mermaid
graph LR
    MODULES["All xKit Modules"] -->|"return"| ERRNO["xErrno"]
    ERRNO -->|"xstrerror()"| MSG["Human-readable string"]
    MSG -->|"xLog()"| LOG["Log output"]

    style ERRNO fill:#4a90d9,color:#fff
    style MSG fill:#50b86c,color:#fff
```

## Implementation Details

### Error Code Values

The error codes are defined as an `int`-based enum (via `XDEF_ENUM`), starting from 0:

| Code | Value | Meaning |
| --- | --- | --- |
| `xErrno_Ok` | 0 | Success |
| `xErrno_Unknown` | 1 | Unspecified error (legacy / catch-all) |
| `xErrno_InvalidArg` | 2 | NULL or invalid argument |
| `xErrno_NoMemory` | 3 | Memory allocation failed |
| `xErrno_InvalidState` | 4 | Object is in the wrong state for this call |
| `xErrno_SysError` | 5 | Underlying syscall / OS error |
| `xErrno_NotFound` | 6 | Requested item does not exist |
| `xErrno_AlreadyExists` | 7 | Item already registered / bound |
| `xErrno_Cancelled` | 8 | Operation was cancelled |

### Usage Pattern

The idiomatic xKit error handling pattern:

```c
xErrno err = xSomeFunction(args);
if (err != xErrno_Ok) {
    xLog(false, "operation failed: %s", xstrerror(err));
    return err; // propagate
}
```

### Internal Usage

`xErrno` is used by:

- **event.h** — `xEventMod()`, `xEventDel()`, `xEventWake()`, `xEventLoopTimerCancel()`, `xEventLoopSubmit()`, `xEventLoopWorkCancel()`, `xEventLoopPost()`, `xEventLoopSignalWatch()`
- **timer.h** — `xTimerCancel()`
- **task.h** — `xTaskWait()`, `xTaskCancel()`, `xTaskGroupWait()`
- **socket.h** — `xSocketSetMask()`, `xSocketSetTimeout()`
- **heap.h** — `xHeapPush()`, `xHeapUpdate()`

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xErrno` | `int`-based enum of error codes |

### Enum Values

| Value | Description |
| --- | --- |
| `xErrno_Ok` | Success |
| `xErrno_Unknown` | Unspecified error (legacy / catch-all) |
| `xErrno_InvalidArg` | NULL or invalid argument |
| `xErrno_NoMemory` | Memory allocation failed |
| `xErrno_InvalidState` | Object is in the wrong state for this call |
| `xErrno_SysError` | Underlying syscall / OS error |
| `xErrno_NotFound` | Requested item does not exist |
| `xErrno_AlreadyExists` | Item already registered / bound |
| `xErrno_Cancelled` | Operation was cancelled |

### Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xstrerror` | `const char *xstrerror(xErrno err)` | Return a human-readable error message. Never returns NULL. | **Thread-safe** (returns static strings) |

## Usage Examples

### Error Handling Pattern

```c
#include <stdio.h>
#include <xbase/error.h>
#include <xbase/event.h>

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    xErrno err = xEventMod(loop, NULL, xEvent_Read);
    if (err != xErrno_Ok) {
        fprintf(stderr, "xEventMod failed: %s\n", xstrerror(err));
        // Output: "xEventMod failed: NULL or invalid argument"
    }

    xEventLoopDestroy(loop);
    return 0;
}
```

### Propagating Errors

```c
#include <xbase/error.h>
#include <xbase/socket.h>

xErrno setup_socket(xEventLoop loop, xSocket *out) {
    xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                  xEvent_Read, my_callback, NULL);
    if (!sock) return xErrno_SysError;

    xErrno err = xSocketSetTimeout(sock, 5000, 0);
    if (err != xErrno_Ok) {
        xSocketDestroy(loop, sock);
        return err;
    }

    *out = sock;
    return xErrno_Ok;
}
```

## Use Cases

1. **Uniform Error Propagation** — Functions return `xErrno` and callers check against `xErrno_Ok`. This eliminates the need for module-specific error types.

2. **Logging and Diagnostics** — `xstrerror()` provides instant human-readable messages for log output without maintaining separate message tables.

3. **Error Classification** — Callers can switch on specific error codes to implement different recovery strategies (e.g., retry on `xErrno_SysError`, abort on `xErrno_NoMemory`).

## Best Practices

- **Always check return values.** Functions that return `xErrno` should be checked. Functions that return handles (pointers) should be checked for NULL.
- **Use `xstrerror()` in log messages.** It's more informative than printing the raw integer.
- **Don't compare against raw integers.** Always use the enum constants (`xErrno_Ok`, `xErrno_InvalidArg`, etc.) for readability and forward compatibility.
- **Prefer specific codes over `xErrno_Unknown`.** When adding new error paths, choose the most specific applicable code.

## Comparison with Other Libraries

| Feature | xbase error.h | POSIX errno | Windows HRESULT | GLib GError |
| --- | --- | --- | --- | --- |
| **Type** | `int` enum | `int` (thread-local) | `LONG` | Struct (domain + code + message) |
| **Scope** | Library-wide | System-wide | System-wide | Per-domain |
| **String Conversion** | `xstrerror()` | `strerror()` | `FormatMessage()` | `g_error->message` |
| **Thread Safety** | Return value (inherently safe) | Thread-local global | Return value | Heap-allocated |
| **Extensibility** | Add to enum | Platform-defined | Facility codes | Custom domains |
| **Overhead** | Zero (int return) | Zero (thread-local) | Zero (int return) | Heap allocation per error |

**Key Differentiator:** xbase's error system is intentionally simple — a single enum with descriptive codes and a string conversion function. It avoids the complexity of domain-based systems (GError) and the thread-local pitfalls of POSIX errno, while providing enough granularity for library-level error handling.
