# xjs — Values

## Introduction

An `xJSValueRef` is an opaque handle to a JavaScript value (primitive or object). Every value reachable from host code lives in a per-context **slot pool** that holds the underlying QuickJS reference; the slot itself is reference-counted.

This page covers the type system, value construction, conversion, and — most importantly — the **lifetime rules** that are the single biggest deviation from JavaScriptCore's C API.

## Lifetime Rules

> **Important — read this first.**

In **JSC**, `JSValueRef` is a thin wrapper around a conservatively-scanned JS heap pointer: values live at least as long as the VM stack frame that created them, and `JSValueProtect`/`JSValueUnprotect` pairs only matter if you need to stash a value across a return into JS.

In **xjs**, every `xJSValueRef` handed back from the API carries **one reference** in a slot pool, and **the caller is responsible for releasing it** via [`xJSValueUnprotect()`](#reference-count-helpers). Forgetting to unprotect leaks both the slot and the underlying JS value.

The rules:

| Case | Who owns the ref | Who must release |
| --- | --- | --- |
| Return value of any `xJSValueMake*`, `xJSObjectMake*`, `xJSValue*Copy`, `xJSObjectGetProperty*`, `xJSContextGetGlobalObject`, `xJSObjectCallAsFunction`, `xJSEvaluateScript`, `xJSEvaluateModule`, `xJSAwaitPromise`, … | caller | caller — `xJSValueUnprotect` |
| `xJSValueRef` handed in as a parameter (`value`, `arguments[]`, `thisObject`, …) | caller | caller (callee borrows) |
| `*exception` out-param, when populated | caller | caller — `xJSValueUnprotect` |
| `xJSValueRef` received by a native callback as `arguments[i]` | VM | **do not** release (the VM balances) |

If the same handle is needed twice (e.g. stash it in a C struct and also return it), use `xJSValueProtect` to bump the refcount, and release once for each bump.

### Relationship to GC

While a slot is alive it keeps a QuickJS reference on the underlying `JSValue`, which roots it against the garbage collector. `xJSGarbageCollect(ctx)` forces a full GC pass but only reclaims values that no slot (and no live JS reference) still holds.

### Behavioural consequence: `xJSValueUnprotect` on an un-protected value

Because every public value is born with refcount == 1, plain `xJSValueUnprotect(ctx, v)` is the **standard** release call — it matches JSC's naming but is not optional in xjs. Calling it twice on the same handle without a matching `xJSValueProtect` is a double-free.

## Type System

```c
typedef enum {
  kXJSTypeUndefined = 0,
  kXJSTypeNull      = 1,
  kXJSTypeBoolean   = 2,
  kXJSTypeNumber    = 3,
  kXJSTypeString    = 4,
  kXJSTypeObject    = 5,
  kXJSTypeSymbol    = 6,
} xJSType;
```

### Primitive queries

```c
xJSType xJSValueGetType(xJSContextRef ctx, xJSValueRef value);

bool xJSValueIsUndefined(xJSContextRef, xJSValueRef);
bool xJSValueIsNull     (xJSContextRef, xJSValueRef);
bool xJSValueIsBoolean  (xJSContextRef, xJSValueRef);
bool xJSValueIsNumber   (xJSContextRef, xJSValueRef);
bool xJSValueIsString   (xJSContextRef, xJSValueRef);
bool xJSValueIsSymbol   (xJSContextRef, xJSValueRef);
bool xJSValueIsObject   (xJSContextRef, xJSValueRef);
bool xJSValueIsArray    (xJSContextRef, xJSValueRef);
bool xJSValueIsDate     (xJSContextRef, xJSValueRef);
```

### Class / constructor queries

```c
bool xJSValueIsObjectOfClass(xJSContextRef ctx, xJSValueRef v, xJSClassRef c);
bool xJSValueIsInstanceOfConstructor(xJSContextRef ctx, xJSValueRef v,
                                     xJSObjectRef constructor,
                                     xJSValueRef *exception);
```

### Equality

```c
bool xJSValueIsEqual      (xJSContextRef, xJSValueRef a, xJSValueRef b,
                           xJSValueRef *exception);  // ==
bool xJSValueIsStrictEqual(xJSContextRef, xJSValueRef a, xJSValueRef b); // ===
```

`xJSValueIsEqual` can trigger user-defined coercion (`valueOf`/`toString`) and therefore takes an `exception` out-param. `xJSValueIsStrictEqual` is side-effect-free.

## Value Construction

```c
xJSValueRef xJSValueMakeUndefined(xJSContextRef ctx);
xJSValueRef xJSValueMakeNull     (xJSContextRef ctx);
xJSValueRef xJSValueMakeBoolean  (xJSContextRef, bool);
xJSValueRef xJSValueMakeNumber   (xJSContextRef, double);
xJSValueRef xJSValueMakeString   (xJSContextRef, xJSStringRef);
xJSValueRef xJSValueMakeSymbol   (xJSContextRef, xJSStringRef description);
```

All builders return a fresh owning reference; release with `xJSValueUnprotect`.

### JSON bridge

```c
xJSValueRef  xJSValueMakeFromJSONString(xJSContextRef ctx, xJSStringRef json);
xJSStringRef xJSValueCreateJSONString   (xJSContextRef ctx, xJSValueRef v,
                                         unsigned indent, xJSValueRef *exc);
```

`xJSValueMakeFromJSONString` returns `NULL` on parse error (no exception is raised — it is a host-side failure). `xJSValueCreateJSONString` returns `NULL` and sets `*exception` if the value contains cycles or throws from a `toJSON`.

## Conversions

```c
bool         xJSValueToBoolean   (xJSContextRef ctx, xJSValueRef);
double       xJSValueToNumber    (xJSContextRef ctx, xJSValueRef, xJSValueRef *exc);
xJSStringRef xJSValueToStringCopy(xJSContextRef ctx, xJSValueRef, xJSValueRef *exc);
xJSObjectRef xJSValueToObject    (xJSContextRef ctx, xJSValueRef, xJSValueRef *exc);
```

The "`Copy`" in `xJSValueToStringCopy` means **caller owns the returned `xJSStringRef`** and must balance it with `xJSStringRelease`.

Conversions that invoke user code (`toString`, `valueOf`) can throw; non-throwing conversions (`ToBoolean`) do not take an `exception` parameter.

## Reference-count Helpers

```c
void xJSValueProtect  (xJSContextRef ctx, xJSValueRef value);  // +1
void xJSValueUnprotect(xJSContextRef ctx, xJSValueRef value);  // -1 (free at 0)
```

See the [Lifetime Rules](#lifetime-rules) section above. In xjs these are the direct control of the slot refcount, not the "additional root" semantics JSC uses.

## Worked Examples

### Round-trip through JSON

```c
xJSStringRef s = xJSStringCreateWithUTF8CString("{\"x\":1,\"y\":[2,3]}");
xJSValueRef  v = xJSValueMakeFromJSONString(ctx, s);
xJSStringRelease(s);

// … inspect `v` via xJSObjectGetProperty etc …

xJSValueRef  exc = NULL;
xJSStringRef j   = xJSValueCreateJSONString(ctx, v, 2, &exc);

xJSValueUnprotect(ctx, v);
if (j) { /* pretty-printed JSON in `j` */ xJSStringRelease(j); }
```

### Safe number read

```c
xJSValueRef exc = NULL;
double      n   = xJSValueToNumber(ctx, v, &exc);
if (exc) {
    // `v`'s .valueOf threw; print exc and bail
    xJSValueUnprotect(ctx, exc);
}
```

## Caveats

- `xJSValueIsArray` returns true for genuine JS `Array` objects (not for array-like objects with a numeric `length`). Use property inspection if you need the looser test.
- `xJSValueIsDate` only matches `Date` instances created by `new Date(...)`; raw timestamps (numbers) return false.
- Symbols produced via `xJSValueMakeSymbol(ctx, description)` use `Symbol(description)` semantics (non-interned). Use `xJSEvaluateScript(ctx, "Symbol.for('k')", …)` if you need the global registry.
