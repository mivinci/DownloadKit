# xjs — ES Modules

## Introduction

xjs understands ES modules — the `import` / `export` syntax plus top-level `await`. Module support is an **xKit extension** relative to the JavaScriptCore C API (JSC only exposes modules through its private Objective-C surface), but the shape we chose stays close to JSC's `JSModuleLoaderDelegate`.

Key properties:

- Loading is **asynchronous by construction**: `xJSEvaluateModule` returns a Promise that fulfils once every transitive import has loaded and executed.
- Specifier normalisation (resolving `./x` relative to the importer) is handled internally. The loader callback only ever sees **normalised** names.
- **No native-module registration.** xjs does not expose an API for registering a `JSModuleDef` backed by C functions. The recommended pattern is "global hook + JS facade"; see the [example](#example-native-module-facade) below.

## Detecting a Module

Before evaluating a random source blob, decide whether it is a script or a module:

```c
bool xJSDetectModule(const char *source, size_t length);
```

This is a cheap syntactic pre-pass (scans for top-level `import`/`export`) — the same heuristic QuickJS's `JS_DetectModule` applies. Use it to branch between `xJSEvaluateScript` and `xJSEvaluateModule`.

## Evaluating a Module

```c
xJSValueRef xJSEvaluateModule(xJSContextRef ctx,
                              xJSStringRef  script,
                              xJSStringRef  sourceURL,
                              xJSValueRef  *exception);
```

- `script` — module source. Must not be `NULL`.
- `sourceURL` — module identifier. Used as the compile-time source URL (for stack traces and `import.meta.url`) **and** as the base specifier against which relative imports are resolved. Pass `NULL` for the anonymous placeholder `<xjs>`.
- `exception` — populated only for **compile/link-time** failures. Runtime errors — `throw` in top-level code, rejected imports — surface through the returned Promise's rejection path.
- Returns a Promise (as an `xJSValueRef`) on success, or `NULL` on compile/setup error. Release with `xJSValueUnprotect`.

## Awaiting the Result

Because module evaluation is asynchronous, the typical driver pattern is "evaluate, then block until the promise settles":

### `xJSAwaitPromise`

```c
xJSValueRef xJSAwaitPromise(xJSContextRef ctx,
                            xJSValueRef   promise,
                            xJSValueRef  *exception);
```

- Drains pending jobs on `ctx`'s runtime until `promise` leaves the pending state.
- Returns the fulfilment value on resolve; returns `NULL` and sets `*exception` on reject.
- If `promise` is **not** a Promise it is returned as-is with a bumped refcount — this makes the helper safe to wrap around any returned value, even if the backend happens to settle synchronously.
- Detects the "promise never settles" case (queue drained but still pending) and fails loudly with an internal-error exception so host code doesn't spin silently.

`xJSAwaitPromise` is a **general-purpose** helper — not limited to modules. Use it to block on any host-side promise (e.g. one returned from `xJSObjectCallAsFunction` against an `async` function).

## Module Loader Callback

```c
typedef xJSStringRef (*xJSModuleLoadCallback)(xJSContextRef ctx,
                                              const char   *normalizedName,
                                              void         *opaque);

void xJSContextSetModuleLoader(xJSGlobalContextRef   ctx,
                               xJSModuleLoadCallback load,
                               void                 *opaque);
```

- Invoked **once** per normalised specifier per context (xjs caches compiled modules internally — re-imports hit the cache).
- Must return a freshly-created `xJSStringRef` with the module source. xjs takes ownership and releases it after compile.
- Returning `NULL` signals "module not found" — the importing evaluation rejects with a `ReferenceError`.
- `opaque` is the pointer you passed to `xJSContextSetModuleLoader`, handed back unchanged.
- Installing `NULL` as the callback reverts every `import` to the built-in "no loader installed" reject.

### Specifier Normalisation

Relative specifiers (`./x`, `../y/z`) are normalised against the importer's own `sourceURL` before reaching the callback; bare specifiers (`counter`, `@scope/pkg`) are passed through unchanged. If you want custom normalisation (e.g. an alias table), do the rewrite inside your loader when you recognise the bare name.

## End-to-end Driver Pattern

```c
xJSStringRef src = xJSStringCreateWithUTF8CString(user_source);
xJSStringRef url = xJSStringCreateWithUTF8CString("entry.js");
xJSValueRef  exc = NULL;

xJSValueRef promise = xJSEvaluateModule(ctx, src, url, &exc);
xJSStringRelease(src);
xJSStringRelease(url);

if (!promise) {
    // compile/link error
    report_exception(ctx, exc);
    if (exc) xJSValueUnprotect(ctx, exc);
    return;
}

xJSValueRef result = xJSAwaitPromise(ctx, promise, &exc);
xJSValueUnprotect(ctx, promise);

if (!result) {
    // runtime error
    report_exception(ctx, exc);
    if (exc) xJSValueUnprotect(ctx, exc);
    return;
}

// `result` is the module namespace object; release when done
xJSValueUnprotect(ctx, result);
```

## Example: Native Module Facade

The "global hook + JS facade" idiom lets you expose C functions under an ergonomic `import` form without adding any new API surface. Full source lives at [`examples/xjs_native_module.c`](../../../examples/xjs_native_module.c); the essential pieces are:

1. **Register C callbacks on the global object** under a mangled key.

   ```c
   // globalThis.__native_counter = { inc, get, reset };
   install_native(ctx, "__native_counter", "inc",   native_counter_inc);
   install_native(ctx, "__native_counter", "get",   native_counter_get);
   install_native(ctx, "__native_counter", "reset", native_counter_reset);
   ```

2. **Synthesize a tiny JS facade in the loader**:

   ```c
   static xJSStringRef load_native_module(xJSContextRef ctx,
                                          const char *name, void *_) {
       if (strcmp(name, "counter") == 0) {
           static const char src[] =
               "const H = globalThis.__native_counter;\n"
               "export const increment = H.inc;\n"
               "export const get       = H.get;\n"
               "export const reset     = H.reset;\n";
           return xJSStringCreateWithUTF8CString(src);
       }
       return NULL;
   }
   xJSContextSetModuleLoader(ctx, load_native_module, NULL);
   ```

3. User code imports normally:

   ```js
   import { increment, get, reset } from "counter";
   for (let i = 0; i < 3; i++) increment();
   log("count =", get());   // count = 3
   ```

QuickJS handles binding resolution, cycle detection, and top-level `await` on the facade for free — no manual `JSModuleDef` plumbing.

## Best Practices

- **Always route module results through `xJSAwaitPromise`.** Module evaluation returns a Promise even when nothing is async — treating the return value as "just a value" will leave you holding a pending Promise and no result.
- **Give your entry module a real `sourceURL`.** `"entry.js"` (or any path-like name) makes relative imports (`./helper.js`) resolvable and gives users readable stack traces. The `NULL` / `"<xjs>"` placeholder breaks relative imports.
- **Make the loader fast and pure.** It runs synchronously from the compiler; any IO you do inside the loader blocks module compilation. If module sources must come from disk or network, preload them into a host-side cache and have the loader hit that cache.
- **Use the "global hook + JS facade" idiom for native modules.** Until native `JSModuleDef` registration lands, synthesising a small JS facade in the loader is both the recommended and the only portable way. See the [example](#example-native-module-facade) above.
- **Bare specifiers are your alias table's job.** xjs only normalises relative paths; if you want `import x from "foo"` to mean `node_modules/foo/index.js`, do the rewrite in your loader when you see a bare name.
- **Don't share compiled modules across contexts.** The module cache is per-context. If you need hot-path re-import, reuse the same context.

## Caveats

- Module evaluation **always** returns a Promise — even if the module has no `await` and no async imports. Always route through `xJSAwaitPromise` (or your own job-pump loop) to retrieve the result.
- The internal module cache is keyed on normalised names **per context**. Two contexts in the same group do not share compiled modules.
- xjs does not persist compiled byte-code to disk. Every `xJSEvaluateModule` recompiles on the calling context.
- The `sourceURL` you pass to `xJSEvaluateModule` is also the base specifier for relative imports — choose something like `"entry.js"` (not `"<xjs>"`) if your entry module has `import "./helper.js"` statements.
