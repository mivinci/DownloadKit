# xjs — Strings

## Introduction

`xJSStringRef` is xjs's string type for API boundaries — it is **not** a JavaScript string value (use `xJSValueMakeString` for that), but rather the encoding-aware byte bag used by every helper that names a property, loads a module, reports an exception, etc.

Internally a string is a ref-counted UTF-16 buffer; UTF-8 transcoding happens on the way in and out.

## Encoding & Layout

- Storage: UTF-16 code units (`uint16_t[]`), allocated as a single block alongside the header for cache friendliness. The buffer is NUL-terminated so it can be passed to UTF-16-aware APIs directly.
- UTF-8 input (`xJSStringCreateWithUTF8CString`) is transcoded into UTF-16.
- UTF-8 output (`xJSStringGetUTF8CString`) transcodes back. The helper returns the number of bytes **including** the trailing NUL (matching JSC).

The UTF-16 storage is the canonical JS string shape (ES uses UTF-16 for `.length` and indexing), so keeping it native avoids re-transcoding on every property lookup.

## Construction

```c
xJSStringRef xJSStringCreateWithCharacters(const uint16_t *chars, size_t n);
xJSStringRef xJSStringCreateWithUTF8CString(const char *cstr);
```

Both allocate a fresh refcount-1 string. Passing `NULL` to `xJSStringCreateWithUTF8CString` yields a valid empty string (not `NULL`).

## Ref Counting

```c
xJSStringRef xJSStringRetain (xJSStringRef s);
void         xJSStringRelease(xJSStringRef s);
```

Every constructor/copy returns a fresh reference that the caller must balance with exactly one `xJSStringRelease`. Strings handed to API sinks (`xJSObjectSetProperty`, `xJSEvaluateModule`, …) are borrowed — the callee does not take ownership.

## Reading the Buffer

```c
size_t           xJSStringGetLength          (xJSStringRef s);
const uint16_t  *xJSStringGetCharactersPtr   (xJSStringRef s);

size_t xJSStringGetMaximumUTF8CStringSize(xJSStringRef s);
size_t xJSStringGetUTF8CString           (xJSStringRef s,
                                          char *buffer, size_t bufferSize);
```

Typical "get as UTF-8 C string" pattern:

```c
size_t cap = xJSStringGetMaximumUTF8CStringSize(s);
char  *buf = malloc(cap);
size_t n   = xJSStringGetUTF8CString(s, buf, cap);
// buf is NUL-terminated, n includes the NUL
```

The "Maximum" helper reports a safe upper bound (worst case: 3 bytes per code unit + NUL) — ideal as the malloc size. The actual number of bytes written is the `n` returned.

## Equality

```c
bool xJSStringIsEqual               (xJSStringRef a, xJSStringRef b);
bool xJSStringIsEqualToUTF8CString  (xJSStringRef a, const char *b);
```

Both are code-unit-exact comparisons (no normalisation). `IsEqualToUTF8CString` internally transcodes `b` for comparison.

## Relationship with Values and Properties

- `xJSValueRef ↔ xJSStringRef`: use `xJSValueMakeString` / `xJSValueToStringCopy`.
- Property keys in `xJSObjectGetProperty` / `xJSObjectSetProperty` / `xJSObjectHasProperty` are `xJSStringRef`. Build them once, reuse freely.
- Module identifiers and source URLs passed to `xJSEvaluateModule` / `xJSContextSetModuleLoader` are `xJSStringRef` on the way in; the loader callback receives a plain UTF-8 `const char *normalizedName` for convenience.

## Caveats

- xjs does not (yet) expose an API for inspecting UTF-8 byte length independently of the worst-case upper bound. If you need tight sizing, transcode once and measure.
- `xJSStringIsEqualToUTF8CString` allocates on every call (it builds a transient UTF-16 copy). For hot-path comparisons, cache the UTF-16 form with `xJSStringCreateWithUTF8CString` up front.
- There is no string *slice*, *concat*, or *index-of* API at the xjs layer — such operations belong in JS. If you need to manipulate strings in host code, transcode to UTF-8 once and use xbase's [`xString`](../xbase/string.md) helpers.

## Worked Example — Calling with a UTF-8 property name

```c
xJSStringRef k = xJSStringCreateWithUTF8CString("status");
xJSValueRef  v = xJSObjectGetProperty(ctx, obj, k, NULL);
xJSStringRelease(k);

xJSStringRef vs = xJSValueToStringCopy(ctx, v, NULL);
xJSValueUnprotect(ctx, v);

size_t cap = xJSStringGetMaximumUTF8CStringSize(vs);
char  *buf = malloc(cap);
xJSStringGetUTF8CString(vs, buf, cap);
printf("status = %s\n", buf);

free(buf);
xJSStringRelease(vs);
```
