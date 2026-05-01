# xjs — Objects, Functions & Promises

## Introduction

`xJSObjectRef` is a specialisation of `xJSValueRef` restricted to the JavaScript *Object* type — arrays, dates, errors, regexps, functions, constructors, promises, and native class instances all show up as `xJSObjectRef`. Every `xJSObjectRef` is binary-compatible with `xJSValueRef` and follows the same [value lifetime rules](value.md#lifetime-rules-important-read-this-first).

## Creating Objects

### Generic object

```c
xJSObjectRef xJSObjectMake(xJSContextRef ctx, xJSClassRef cls, void *data);
```

`cls == NULL` produces a plain `{}`. Pass a class created by [`xJSClassCreate`](class.md) to wrap a C struct — `data` is stored in the object's private slot and retrieved via [`xJSObjectGetPrivate`](#private-data).

### Host-callable function

```c
xJSObjectRef xJSObjectMakeFunctionWithCallback(
    xJSContextRef ctx, xJSStringRef name,
    xJSObjectCallAsFunctionCallback cb);
```

The returned object is indistinguishable from a JS function (`typeof fn === "function"`, callable from user code).

```c
static xJSValueRef add(xJSContextRef ctx, xJSObjectRef fn, xJSObjectRef thiz,
                       size_t argc, const xJSValueRef argv[],
                       xJSValueRef *exc) {
    (void)fn; (void)thiz;
    double a = argc > 0 ? xJSValueToNumber(ctx, argv[0], exc) : 0;
    double b = argc > 1 ? xJSValueToNumber(ctx, argv[1], exc) : 0;
    return xJSValueMakeNumber(ctx, a + b);
}

xJSStringRef name = xJSStringCreateWithUTF8CString("add");
xJSObjectRef fn   = xJSObjectMakeFunctionWithCallback(ctx, name, add);
xJSStringRelease(name);
```

### Constructor for a native class

```c
xJSObjectRef xJSObjectMakeConstructor(
    xJSContextRef ctx, xJSClassRef cls,
    xJSObjectCallAsConstructorCallback ctor);
```

Registers `cls` against the context's runtime on first use, then returns a function that — when invoked with `new` — calls `ctor`. See [class.md](class.md) for the full flow.

### Compile-at-runtime function

```c
xJSObjectRef xJSObjectMakeFunction(
    xJSContextRef ctx, xJSStringRef name,
    unsigned parameterCount, const xJSStringRef parameterNames[],
    xJSStringRef body, xJSStringRef sourceURL, int startingLineNumber,
    xJSValueRef *exception);
```

Equivalent to `new Function(...parameterNames, body)`. Compile errors surface via `*exception` and a `NULL` return.

### Built-in specialisations

```c
xJSObjectRef xJSObjectMakeArray (xJSContextRef, size_t argc, const xJSValueRef argv[], xJSValueRef *exc);
xJSObjectRef xJSObjectMakeDate  (xJSContextRef, size_t argc, const xJSValueRef argv[], xJSValueRef *exc);
xJSObjectRef xJSObjectMakeError (xJSContextRef, size_t argc, const xJSValueRef argv[], xJSValueRef *exc);
xJSObjectRef xJSObjectMakeRegExp(xJSContextRef, size_t argc, const xJSValueRef argv[], xJSValueRef *exc);
```

Each is a thin shortcut for `new Array(...)` / `new Date(...)` / etc.

### Deferred promise (for async host work)

```c
xJSObjectRef xJSObjectMakeDeferredPromise(
    xJSContextRef ctx,
    xJSObjectRef *resolve, xJSObjectRef *reject,
    xJSValueRef  *exception);
```

Returns a pending Promise plus its `resolve`/`reject` functions. The typical flow:

1. Kick off async work in host land; capture `ctx`, `resolve`, `reject`.
2. Return the promise to JavaScript.
3. When the work completes, call `xJSObjectCallAsFunction(ctx, resolve, NULL, 1, &result, &exc);` (or `reject`).
4. **Call `xJSContextDrainPendingJobs(ctx, …)`** so the `.then` reactions run.
5. Release the three `xJSObjectRef` handles once you no longer need them.

## Accessing Object Properties

### By string key

```c
bool        xJSObjectHasProperty    (xJSContextRef, xJSObjectRef, xJSStringRef);
xJSValueRef xJSObjectGetProperty    (xJSContextRef, xJSObjectRef, xJSStringRef, xJSValueRef *exc);
void        xJSObjectSetProperty    (xJSContextRef, xJSObjectRef, xJSStringRef,
                                     xJSValueRef value,
                                     xJSPropertyAttributes attrs,
                                     xJSValueRef *exc);
bool        xJSObjectDeleteProperty (xJSContextRef, xJSObjectRef, xJSStringRef, xJSValueRef *exc);
```

Attribute flags (bit-ORed into `attrs`):

```c
kXJSPropertyAttributeNone       = 0
kXJSPropertyAttributeReadOnly   = 1 << 1
kXJSPropertyAttributeDontEnum   = 1 << 2
kXJSPropertyAttributeDontDelete = 1 << 3
```

### By integer index

```c
xJSValueRef xJSObjectGetPropertyAtIndex(xJSContextRef, xJSObjectRef,
                                        unsigned idx, xJSValueRef *exc);
void        xJSObjectSetPropertyAtIndex(xJSContextRef, xJSObjectRef,
                                        unsigned idx, xJSValueRef value,
                                        xJSValueRef *exc);
```

Faster than the string variant for arrays and typed arrays.

### Enumeration

```c
xJSPropertyNameArrayRef names = xJSObjectCopyPropertyNames(ctx, obj);
size_t n = xJSPropertyNameArrayGetCount(names);
for (size_t i = 0; i < n; ++i) {
    xJSStringRef k = xJSPropertyNameArrayGetNameAtIndex(names, i);
    // … inspect k …
}
xJSPropertyNameArrayRelease(names);
```

Only own, enumerable, string-keyed properties are listed (matching `Object.keys`). Symbol keys require lowering into JS (`Reflect.ownKeys(...)`).

## Prototype

```c
xJSValueRef xJSObjectGetPrototype(xJSContextRef, xJSObjectRef);
void        xJSObjectSetPrototype(xJSContextRef, xJSObjectRef, xJSValueRef proto);
```

Pass `xJSValueMakeNull(ctx)` to detach the prototype.

## Calling Functions and Constructors

```c
bool        xJSObjectIsFunction    (xJSContextRef, xJSObjectRef);
xJSValueRef xJSObjectCallAsFunction(xJSContextRef, xJSObjectRef fn,
                                    xJSObjectRef thisObj,
                                    size_t argc, const xJSValueRef argv[],
                                    xJSValueRef *exception);

bool         xJSObjectIsConstructor    (xJSContextRef, xJSObjectRef);
xJSObjectRef xJSObjectCallAsConstructor(xJSContextRef, xJSObjectRef ctor,
                                        size_t argc, const xJSValueRef argv[],
                                        xJSValueRef *exception);
```

Passing `thisObj == NULL` in `CallAsFunction` uses `globalThis` as `this`, matching JSC.

## Private Data

For instances of a class created via [`xJSClassCreate`](class.md), an opaque `void *` slot is available:

```c
void *xJSObjectGetPrivate(xJSObjectRef obj);
bool  xJSObjectSetPrivate(xJSObjectRef obj, void *data);
```

`Set` returns `false` when called on a plain object (no class → no private slot). The private pointer is handed back from the `finalize` callback so you can free it; xjs does **not** take ownership of it.

## Worked Example — Call a JS function from C

```c
// const x = 5;  add(x, 7)  →  12

xJSStringRef nameK = xJSStringCreateWithUTF8CString("add");
xJSObjectRef g     = xJSContextGetGlobalObject(ctx);
xJSValueRef  fn    = xJSObjectGetProperty(ctx, g, nameK, NULL);
xJSValueUnprotect(ctx, (xJSValueRef)g);
xJSStringRelease(nameK);

xJSValueRef args[2] = {
    xJSValueMakeNumber(ctx, 5),
    xJSValueMakeNumber(ctx, 7),
};
xJSValueRef exc = NULL;
xJSValueRef r   = xJSObjectCallAsFunction(ctx, (xJSObjectRef)fn, NULL,
                                          2, args, &exc);
for (int i = 0; i < 2; ++i) xJSValueUnprotect(ctx, args[i]);
xJSValueUnprotect(ctx, fn);

if (!r) { /* exc populated */ }
else {
    printf("add(5,7) = %g\n", xJSValueToNumber(ctx, r, NULL));
    xJSValueUnprotect(ctx, r);
}
```

## Caveats

- Native callbacks are invoked **synchronously** from JS. Long-running work must be offloaded to a host thread and surfaced via `xJSObjectMakeDeferredPromise` so JS stays responsive.
- Returning one of the incoming `argv[i]` (or `thisObject`/`function`) from a callback is supported — xjs detects the aliasing and does not double-release. Returning a freshly built value is also fine; the wrapper extracts the underlying `JSValue` and releases the slot for you.
- `xJSPropertyNameArrayRef` owns a retained copy of each name; the strings returned by `GetNameAtIndex` are alive for as long as the array is. Do not `xJSStringRelease` them directly.
