# string.h — SDS-Style Dynamic String

## Introduction

`string.h` provides an SDS-style dynamic string (`XString`) that is fully compatible with all C string functions (`printf %s`, `strcmp`, `strlen`, …). The header (length + capacity) is hidden before the user-facing pointer, so every `XString` **is** a `char*` — zero interop friction.

Inspired by Redis SDS (Simple Dynamic Strings).

Typical usage:

```c
XString s = XStringCreate("hello");
s = XStringAppend(s, " world");
printf("%s (len=%zu)\n", s, XStringLen(s));

size_t pos = XStringFindStr(s, "world");
if (pos != XSTRING_NONE) {
  printf("found at index %zu\n", pos);
}

XStringDestroy(s);
```

## Design Philosophy

1. **Binary-Compatible with C Strings** — `XString` is a `typedef char *`. Every XString can be passed directly to any C string API without conversion. It is always NUL-terminated.

2. **Hidden Header** — The metadata (length, capacity) lives in a header placed *before* the user pointer. This means `XString` is indistinguishable from a regular `char*` at the call site, yet length queries are O(1).

3. **Auto-Growing** — Append operations automatically reallocate when capacity is exhausted. Callers must use the return value (`s = XStringAppend(s, "x")`) because reallocation may move the string.

4. **Binary-Safe** — Embedded NUL bytes are supported. `XStringCreateLen` and `XStringAppendLen` treat the input as raw bytes. Length is tracked explicitly, not via `strlen`.

5. **Dual-Strategy Search** — `XStringFind` uses naive `memcmp` for short patterns (below a threshold) and platform `memmem` for longer ones, balancing call overhead against algorithmic advantage.

## Architecture

```mermaid
graph TD
    CREATE["XStringCreate(init)"] --> S["XString<br/>(char*)"]
    CREATELEN["XStringCreateLen(data, len)"] --> S
    APPEND["XStringAppend(s, str)"] --> GROW["Grow if needed"]
    APPENDLEN["XStringAppendLen(s, data, len)"] --> GROW
    APPENDFMT["XStringAppendFormat(s, fmt, ...)"] --> GROW
    GROW --> UPDATE["Return updated pointer"]
    FIND["XStringFind(haystack, needle, len)"] --> THRESH{"needle_len < 32?"}
    THRESH -->|Yes| NAIVE["Naive memcmp scan"]
    THRESH -->|No| MEMMEM["memmem (platform Two-Way)"]
    DUP["XStringDup(s)"] --> S
    TRUNCATE["XStringTruncate(s, new_len)"] --> S
    CLEAR["XStringClear(s)"] --> S
    DESTROY["XStringDestroy(s)"] --> FREE["free(header + data)"]

    S --> APPEND
    S --> APPENDLEN
    S --> APPENDFMT
    S --> FIND
    S --> DUP
    S --> TRUNCATE
    S --> CLEAR
    S --> DESTROY

    style CREATE fill:#4a90d9,color:#fff
    style CREATELEN fill:#4a90d9,color:#fff
    style APPEND fill:#50b86c,color:#fff
    style APPENDLEN fill:#50b86c,color:#fff
    style APPENDFMT fill:#50b86c,color:#fff
    style FIND fill:#f5a623,color:#fff
    style DESTROY fill:#e74c3c,color:#fff
```

## Implementation Details

### Memory Layout

```text
                    XStringHeader
                 ┌──────────────┐
                 │ len (size_t) │
                 │ cap (size_t) │
                 └──────────────┘ ← hdr + 1 = user pointer
                 ┌──────────────┐
  XString (char*) → │  data …      │ ← always NUL-terminated
                 │  cap + 1     │
                 └──────────────┘
```

The `XStringHeader` is allocated as part of a single `malloc` block: `malloc(sizeof(XStringHeader) + cap + 1)`. The user receives a pointer to the data area, which is `(XStringHeader*)ptr + 1`. This layout means:

- `XStringLen(s)` is O(1) — reads `hdr->len` directly.
- `s` can be passed to any `const char*` API.
- The NUL terminator is always written after `len` bytes.

### Growth Strategy

When an append exceeds current capacity:

1. If current capacity < 1 MB → **double** the capacity.
2. If current capacity ≥ 1 MB → **add 1 MB**.
3. Minimum capacity is `XSTRING_MIN_CAP = 64` bytes.

This mirrors the Redis SDS growth policy and provides good amortised O(1) appends without wasting memory on large strings.

### Search Strategy

`xStringFind` uses a threshold-based approach:

| Pattern Length | Algorithm | Rationale |
| --- | --- | --- |
| `< XSTRING_FIND_THRESHOLD` (32) | Naive `memcmp` scan | Avoids `memmem` call overhead for short patterns where O(n·m) is negligible. |
| `≥ XSTRING_FIND_THRESHOLD` | Platform `memmem` | Leverages glibc's Two-Way algorithm (O(n+m) worst case) or equivalent. |

Not-found results return `XSTRING_NONE` (`(size_t)-1`), consistent with the `ARRAY_NPOS` convention used elsewhere in xbase.

### Operations and Complexity

| Operation | Function | Time Complexity | Description |
| --- | --- | --- | --- |
| Create | `xStringCreate` | O(n) | Copy init string + allocate header |
| Create (binary) | `xStringCreateLen` | O(n) | Copy n bytes + allocate header |
| Destroy | `xStringDestroy` | O(1) | Free the single allocation |
| Duplicate | `xStringDup` | O(n) | Copy all data into new allocation |
| Append | `xStringAppend` | Amortised O(n) | May realloc, then memcpy |
| Append (binary) | `xStringAppendLen` | Amortised O(n) | May realloc, then memcpy |
| Append (format) | `xStringAppendFormat` | Amortised O(n) | vsnprintf into available space; grow + retry if needed |
| Truncate | `xStringTruncate` | O(1) | Write NUL, update len |
| Clear | `xStringClear` | O(1) | Write NUL at index 0, set len = 0 |
| Length | `xStringLen` | O(1) | Read header field |
| Capacity | `xStringCap` | O(1) | Read header field |
| Available | `xStringAvail` | O(1) | cap − len |
| Grow | `xStringGrow` | O(n) | Pre-allocate, may realloc |
| Shrink to fit | `xStringShrinkToFit` | O(n) | realloc to exact size |
| Find | `xStringFind` | O(n·m) or O(n+m) | Threshold-based: naive or memmem |
| Find (C string) | `xStringFindStr` | O(n·m) or O(n+m) | Delegates to `xStringFind` |
| Compare | `xStringCmp` | O(n) | Binary-safe memcmp |
| Equal | `xStringEq` | O(n) | `xStringCmp == 0` |

## API Reference

### Types and Constants

| Type / Constant | Description |
| --- | --- |
| `xString` | `typedef char *`. SDS-style dynamic string, compatible with all C string APIs. |
| `XSTRING_NONE` | `((size_t)-1)`. Sentinel returned by `xStringFind` / `xStringFindStr` when the needle is not found. |

### Lifecycle Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringCreate` | `xString xStringCreate(const char *init)` | Create from C string. `init` may be NULL (→ empty). | Not thread-safe |
| `xStringCreateLen` | `xString xStringCreateLen(const void *init, size_t len)` | Create from raw memory (binary-safe). `init` may be NULL if len == 0. | Not thread-safe |
| `xStringDestroy` | `void xStringDestroy(xString s)` | Free the string. NULL is a no-op. | Not thread-safe |
| `xStringDup` | `xString xStringDup(const xString s)` | Deep copy. NULL → NULL. | Not thread-safe |

### Append Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringAppend` | `xString xStringAppend(xString s, const char *append)` | Append C string. May realloc; use return value. | Not thread-safe |
| `xStringAppendLen` | `xString xStringAppendLen(xString s, const void *append, size_t len)` | Append raw bytes (binary-safe). | Not thread-safe |
| `xStringAppendFormat` | `xString xStringAppendFormat(xString s, const char *fmt, ...)` | Append printf-style formatted string. | Not thread-safe |

### Truncate / Clear

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringTruncate` | `void xStringTruncate(xString s, size_t new_len)` | Shorten to `new_len`. No-op if `new_len > len`. Does not shrink allocation. | Not thread-safe |
| `xStringClear` | `void xStringClear(xString s)` | Reset to empty string `""`. Does not shrink allocation. | Not thread-safe |

### Accessor Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringLen` | `size_t xStringLen(const xString s)` | String length in O(1). NULL → 0. | Not thread-safe |
| `xStringCap` | `size_t xStringCap(const xString s)` | Allocated capacity. NULL → 0. | Not thread-safe |
| `xStringAvail` | `size_t xStringAvail(const xString s)` | Available space = cap − len. NULL → 0. | Not thread-safe |

### Memory Control Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringGrow` | `xString xStringGrow(xString s, size_t add_len)` | Pre-allocate for `add_len` more bytes. Does not change length. | Not thread-safe |
| `xStringShrinkToFit` | `xString xStringShrinkToFit(xString s)` | Realloc to fit content exactly. On failure, keeps original allocation. | Not thread-safe |

### Search Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringFind` | `size_t xStringFind(const xString haystack, const char *needle, size_t needle_len)` | Binary-safe search. Returns byte index or `XSTRING_NONE`. | Not thread-safe |
| `xStringFindStr` | `size_t xStringFindStr(const xString haystack, const char *needle)` | C string search. Equivalent to `xStringFind(haystack, needle, strlen(needle))`. Returns byte index or `XSTRING_NONE`. | Not thread-safe |

### Comparison Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xStringCmp` | `int xStringCmp(const xString s1, const xString s2)` | Binary-safe comparison. Returns <0, 0, >0. NULL sorts before non-NULL. | Not thread-safe |
| `xStringEq` | `int xStringEq(const xString s1, const xString s2)` | Returns non-zero if equal. NULL == NULL is true. | Not thread-safe |

## Usage Examples

### Basic Create / Append / Destroy

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  xString s = xStringCreate("hello");
  s = xStringAppend(s, " world");

  printf("%s (len=%zu, cap=%zu)\n", s, xStringLen(s), xStringCap(s));
  /* Output: hello world (len=11, cap=64) */

  xStringDestroy(s);
  return 0;
}
```

### Binary-Safe String (Embedded NUL)

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  char data[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f' };
  xString s = xStringCreateLen(data, 7);

  printf("len=%zu\n", xStringLen(s));  /* len=7, NOT 3 */

  size_t pos = xStringFind(s, "def", 3);
  if (pos != XSTRING_NONE) {
    printf("found 'def' at index %zu\n", pos);  /* found 'def' at index 4 */
  }

  xStringDestroy(s);
  return 0;
}
```

### Formatted Append

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  xString s = xStringCreate("count: ");
  s = xStringAppendFormat(s, "%d items", 42);

  printf("%s\n", s);  /* count: 42 items */

  xStringDestroy(s);
  return 0;
}
```

### Search with XSTRING_NONE

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  xString s = xStringCreate("the quick brown fox");

  size_t pos = xStringFindStr(s, "brown");
  if (pos != XSTRING_NONE) {
    printf("'brown' at index %zu\n", pos);  /* 'brown' at index 10 */
  }

  pos = xStringFindStr(s, "cat");
  if (pos == XSTRING_NONE) {
    printf("'cat' not found\n");
  }

  xStringDestroy(s);
  return 0;
}
```

### Pre-allocation and Shrink

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  xString s = xStringCreate("hello");

  /* Pre-allocate 1 KB to avoid repeated reallocs. */
  s = xStringGrow(s, 1024);
  printf("avail=%zu\n", xStringAvail(s));  /* >= 1024 */

  s = xStringAppend(s, " world");
  s = xStringShrinkToFit(s);
  printf("cap=%zu, len=%zu\n", xStringCap(s), xStringLen(s));
  /* cap=11, len=11 */

  xStringDestroy(s);
  return 0;
}
```

### Comparison and Equality

```c
#include <stdio.h>
#include <x/base/string.h>

int main(void) {
  xString a = xStringCreate("abc");
  xString b = xStringCreate("abc");
  xString c = xStringCreate("abd");

  printf("a == b: %d\n", xStringEq(a, b));   /* 1 (true) */
  printf("a == c: %d\n", xStringEq(a, c));   /* 0 (false) */
  printf("a cmp c: %d\n", xStringCmp(a, c)); /* <0 */

  xStringDestroy(a);
  xStringDestroy(b);
  xStringDestroy(c);
  return 0;
}
```

## Use Cases

1. **Network Protocol Buffers** — xString's binary safety and O(1) length make it ideal for building wire-format messages (HTTP headers, WebSocket frames, STUN attributes) where embedded NULs occur and `strlen` is unreliable.

2. **Log Message Assembly** — `xStringAppendFormat` provides a convenient way to build structured log lines incrementally, with automatic growth and no fixed-size buffer overflow risk.

3. **Configuration String Handling** — xString can hold user-provided configuration values, supporting both C-string APIs and explicit-length operations. `xStringFindStr` enables simple key-value parsing.

4. **General String Builder** — Any module that needs to concatenate multiple strings or formatted output can use xString as a safer, more ergonomic alternative to manual `malloc`/`realloc`/`snprintf` management.

## Best Practices

- **Always use the return value from append/grow functions.** `s = xStringAppend(s, "x")` — the pointer may change after reallocation. The old pointer remains valid on failure, so you can still use it, but the new data won't be appended.
- **Use `XSTRING_NONE` to check search results.** `if (xStringFindStr(s, "key") != XSTRING_NONE)` is clearer and more idiomatic than comparing against `(size_t)-1`.
- **Prefer `xStringCreateLen` for binary data.** `xStringCreate` uses `strlen` internally and will stop at the first NUL byte. `xStringCreateLen` copies exactly the bytes you specify.
- **Use `xStringClear` instead of Destroy+Create for reuse.** `xStringClear` resets to an empty string while preserving the allocated capacity, avoiding a fresh allocation cycle.
- **Pre-allocate with `xStringGrow` for known sizes.** If you know the approximate final size, `xStringGrow` avoids multiple intermediate reallocations during incremental appends.
- **Don't store derived pointers across mutations.** Pointers obtained from the `xString` (e.g. `s + offset`) are invalidated by any append or grow operation that triggers reallocation.

## Comparison with Other Libraries

| Feature | xbase string.h | Redis SDS | C++ `std::string` | bstring |
| --- | --- | --- | --- | --- |
| **Style** | `char*` typedef | `char*` typedef | Class | Opaque struct |
| **Language** | C99 | C | C++ | C |
| **C String Compatible** | Yes | Yes | No (`.c_str()`) | No |
| **Binary-Safe** | Yes | Yes | Yes | Yes |
| **O(1) Length** | Yes | Yes | Yes | Yes |
| **Auto-Growing Append** | Yes | Yes | Yes | Yes |
| **Formatted Append** | `xStringAppendFormat` | `sdscatprintf` | `std::format_to` | No built-in |
| **Search** | `xStringFind` (threshold) | `strstr` only | `find()` | `bfind` |
| **Thread Safety** | Not thread-safe | Not thread-safe | Not thread-safe | Not thread-safe |

**Key Differentiator:** xString combines Redis SDS's zero-friction `char*` compatibility with a threshold-based search strategy and `printf`-style formatted append — a practical middle ground between the minimalism of Redis SDS and the full feature set of C++ `std::string`.
