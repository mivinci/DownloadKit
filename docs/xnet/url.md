# url.h — Lightweight URL Parser

## Introduction

`url.h` provides `xUrl`, a lightweight URL parser that
decomposes a URL string into its RFC 3986 components:
scheme, userinfo, host, port, path, query, and fragment.
The parser makes a single internal copy of the input; all
component fields are pointer+length pairs referencing this
copy, so the caller may discard the original string
immediately after parsing.

## Design Philosophy

1. **Single Copy, Zero Per-Field Allocation** — `xUrlParse()`
   calls `strdup()` once. All output fields point into this
   copy, avoiding per-component heap allocations.

2. **Pointer+Length Pairs** — Fields use `const char *` +
   `size_t` pairs rather than NUL-terminated strings. This
   avoids mutating the internal copy and supports efficient
   substring access.

3. **Scheme-Aware Default Ports** — `xUrlPort()` returns
   well-known default ports (80 for http/ws, 443 for
   https/wss) when no explicit port is present, simplifying
   connection logic.

4. **IPv6 Literal Support** — The parser correctly handles
   bracketed IPv6 addresses (`[::1]:8080`), extracting the
   bare address without brackets.

## Architecture

```mermaid
flowchart LR
    INPUT["Raw URL string"]
    PARSE["xUrlParse()"]
    COPY["strdup() internal copy"]
    FIELDS["Pointer+Length fields"]
    PORT["xUrlPort()"]
    FREE["xUrlFree()"]

    INPUT --> PARSE
    PARSE --> COPY
    COPY --> FIELDS
    FIELDS --> PORT
    FIELDS --> FREE

    style PARSE fill:#4a90d9,color:#fff
    style FREE fill:#e74c3c,color:#fff
```

## Implementation Details

### URL Format

```text
scheme://[userinfo@]host[:port][/path][?query][#fragment]
```

### Parsing Steps

```mermaid
flowchart TD
    START["Input: raw URL string"]
    SCHEME["Find '://' → extract scheme"]
    AUTH["Parse authority section"]
    USERINFO{"Contains '@'?"}
    UI_YES["Extract userinfo"]
    HOST{"Starts with '['?"}
    IPV6["Parse IPv6 bracket literal"]
    IPV4["Scan backwards for ':'"]
    PORT["Extract port (if present)"]
    PATH{"Starts with '/'?"}
    PATH_YES["Extract path"]
    QUERY{"Starts with '?'?"}
    QUERY_YES["Extract query"]
    FRAG{"Starts with '#'?"}
    FRAG_YES["Extract fragment"]
    DONE["Return xErrno_Ok"]

    START --> SCHEME --> AUTH
    AUTH --> USERINFO
    USERINFO -->|Yes| UI_YES --> HOST
    USERINFO -->|No| HOST
    HOST -->|Yes| IPV6 --> PORT
    HOST -->|No| IPV4 --> PORT
    PORT --> PATH
    PATH -->|Yes| PATH_YES --> QUERY
    PATH -->|No| QUERY
    QUERY -->|Yes| QUERY_YES --> FRAG
    QUERY -->|No| FRAG
    FRAG -->|Yes| FRAG_YES --> DONE
    FRAG -->|No| DONE

    style DONE fill:#50b86c,color:#fff
```

### Memory Layout

```text
xUrl struct (stack or heap):
┌──────────┬──────────────────────────────────┐
│  raw_    │→ strdup("https://host:443/path") │
│  scheme  │→ ───────┘                        │
│  host    │→ ──────────────┘                 │
│  port    │→ ───────────────────┘            │
│  path    │→ ────────────────────────┘       │
│  ...     │                                  │
└──────────┴──────────────────────────────────┘
All pointers reference the single raw_ copy.
```

### Operations and Complexity

| Operation | Complexity | Notes |
| --- | --- | --- |
| `xUrlParse` | O(n) | Single pass over the URL string |
| `xUrlPort` | O(1) | Converts port string or returns default |
| `xUrlFree` | O(1) | Frees the internal copy, zeroes struct |

## API Reference

### Lifecycle

| Function | Signature | Description |
| --- | --- | --- |
| `xUrlParse` | `xErrno xUrlParse(const char *raw, xUrl *url)` | Parse a URL into components |
| `xUrlFree` | `void xUrlFree(xUrl *url)` | Free internal copy, zero all fields |

### Query

| Function | Signature | Description |
| --- | --- | --- |
| `xUrlPort` | `uint16_t xUrlPort(const xUrl *url)` | Numeric port (explicit or default by scheme) |

### xUrl Fields

| Field | Type | Description |
| --- | --- | --- |
| `scheme` / `scheme_len` | `const char *` / `size_t` | e.g. `"https"` |
| `userinfo` / `userinfo_len` | `const char *` / `size_t` | e.g. `"user:pass"` (optional) |
| `host` / `host_len` | `const char *` / `size_t` | e.g. `"example.com"` or `"::1"` |
| `port` / `port_len` | `const char *` / `size_t` | e.g. `"8443"` (optional) |
| `path` / `path_len` | `const char *` / `size_t` | e.g. `"/ws/chat"` (optional) |
| `query` / `query_len` | `const char *` / `size_t` | e.g. `"key=val"` (optional) |
| `fragment` / `fragment_len` | `const char *` / `size_t` | e.g. `"section1"` (optional) |

> **Note:** Optional fields have `ptr=NULL, len=0` when
> absent. The `raw_` field is internal — do not access it.

## Usage Examples

### Basic URL Parsing

```c
#include <stdio.h>
#include <xnet/url.h>

int main(void) {
    xUrl url;
    xErrno err = xUrlParse(
        "https://user:pass@example.com:8443"
        "/ws/chat?token=abc#top",
        &url);
    if (err != xErrno_Ok) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }

    printf("scheme:   %.*s\n",
           (int)url.scheme_len, url.scheme);
    printf("userinfo: %.*s\n",
           (int)url.userinfo_len, url.userinfo);
    printf("host:     %.*s\n",
           (int)url.host_len, url.host);
    printf("port:     %.*s (numeric: %u)\n",
           (int)url.port_len, url.port,
           xUrlPort(&url));
    printf("path:     %.*s\n",
           (int)url.path_len, url.path);
    printf("query:    %.*s\n",
           (int)url.query_len, url.query);
    printf("fragment: %.*s\n",
           (int)url.fragment_len, url.fragment);

    xUrlFree(&url);
    return 0;
}
```

Output:

```text
scheme:   https
userinfo: user:pass
host:     example.com
port:     8443 (numeric: 8443)
path:     /ws/chat
query:    token=abc
fragment: top
```

### IPv6 Address

```c
xUrl url;
xUrlParse("http://[::1]:8080/test", &url);

printf("host: %.*s\n",
       (int)url.host_len, url.host);
// Output: host: ::1  (brackets stripped)

printf("port: %u\n", xUrlPort(&url));
// Output: port: 8080

xUrlFree(&url);
```

### Default Port by Scheme

```c
xUrl url;
xUrlParse("wss://echo.example.com/sock", &url);

// No explicit port in URL
printf("port field: %s\n",
       url.port ? "present" : "absent");
// Output: port field: absent

// xUrlPort() returns 443 for wss://
printf("effective port: %u\n", xUrlPort(&url));
// Output: effective port: 443

xUrlFree(&url);
```

### Ownership Semantics

```c
// xUrl owns its data — the original string can be freed
char *heap = strdup("ws://example.com:9090/ws");
xUrl url;
xUrlParse(heap, &url);
free(heap);  // safe: xUrl has its own copy

// url fields are still valid here
printf("host: %.*s\n",
       (int)url.host_len, url.host);

xUrlFree(&url);
// After free, all fields are zeroed (NULL)
```

## Error Handling

| Input | Result |
| --- | --- |
| `NULL` raw or url pointer | `xErrno_InvalidArg` |
| Missing `://` separator | `xErrno_InvalidArg` |
| Empty host (e.g. `http:///path`) | `xErrno_InvalidArg` |
| Unclosed IPv6 bracket | `xErrno_InvalidArg` |
| `malloc` failure | `xErrno_NoMemory` |

On error, the `xUrl` struct is zeroed — no cleanup needed.

## Best Practices

- **Always check the return value** of `xUrlParse()`.
  On error the struct is zeroed, so accessing fields is
  safe but yields empty values.
- **Use `xUrlPort()` instead of parsing the port string
  yourself.** It handles default ports and validates the
  numeric range (0–65535).
- **Call `xUrlFree()` when done.** Forgetting to free
  leaks the internal string copy.
- **Don't cache field pointers past `xUrlFree()`.** All
  pointers become invalid after the free call.
