# xjs — Script Evaluation

## Introduction

xjs evaluates JavaScript in two flavours — **classic scripts** (global code, no `import`/`export`) and **ES modules** (covered in [module.md](module.md)). This page focuses on the script path plus the shared job/GC machinery.

## Check Syntax Only

```c
bool xJSCheckScriptSyntax(xJSContextRef ctx, xJSStringRef script,
                          xJSStringRef sourceURL,
                          int startingLineNumber,
                          xJSValueRef *exception);
```

Compiles `script` with `JS_EVAL_FLAG_COMPILE_ONLY` and throws the compiled byte-code away. Use this to validate user input (e.g. an in-app script editor) without running any code. On failure the compile error is reported through `*exception` and the function returns `false`.

## Evaluate a Script

```c
xJSValueRef xJSEvaluateScript(xJSContextRef ctx,
                              xJSStringRef  script,
                              xJSObjectRef  thisObject,
                              xJSStringRef  sourceURL,
                              int           startingLineNumber,
                              xJSValueRef  *exception);
```

- `script` — source code (UTF-16 internally, transcoded to UTF-8 for the compiler).
- `thisObject` — binds `this` at top level. Pass `NULL` to use `globalThis` (JSC-equivalent default).
- `sourceURL` — shows up in stack traces. Pass `NULL` for the default placeholder `<xjs>`.
- `startingLineNumber` — currently accepted but ignored by the QuickJS backend; keep it at `0` or `1` for future compatibility.
- Returns a fresh `xJSValueRef` (release with `xJSValueUnprotect`) or `NULL` on throw (`*exception` is populated).

### Example

```c
xJSStringRef src = xJSStringCreateWithUTF8CString(
    "const a = 2, b = 3;\n"
    "a * b;");
xJSStringRef url = xJSStringCreateWithUTF8CString("calc.js");
xJSValueRef  exc = NULL;

xJSValueRef r = xJSEvaluateScript(ctx, src, NULL, url, 1, &exc);

xJSStringRelease(src);
xJSStringRelease(url);

if (!r) {
    // exc holds the thrown value
    xJSValueUnprotect(ctx, exc);
} else {
    printf("result = %g\n", xJSValueToNumber(ctx, r, NULL));  // 6
    xJSValueUnprotect(ctx, r);
}
```

### Binding `this` at top level

Host code sometimes wants scripts to run against a sandbox object:

```c
xJSObjectRef sandbox = xJSObjectMake(ctx, NULL, NULL);
xJSStringRef hello   = xJSStringCreateWithUTF8CString("hello");
xJSValueRef  v       = xJSValueMakeNumber(ctx, 42);
xJSObjectSetProperty(ctx, sandbox, hello, v, 0, NULL);
xJSValueUnprotect(ctx, v);
xJSStringRelease(hello);

// inside the script, `this.hello` is 42
xJSStringRef src = xJSStringCreateWithUTF8CString("this.hello + 1");
xJSValueRef  r   = xJSEvaluateScript(ctx, src, sandbox, NULL, 0, NULL);
xJSStringRelease(src);
```

## Pumping Async Jobs

QuickJS queues Promise reactions and `queueMicrotask` callbacks on a runtime-level job list, and only executes them when the host explicitly pumps:

```c
int  xJSContextDrainPendingJobs(xJSContextRef ctx, xJSValueRef *exception);
bool xJSContextHasPendingJobs  (xJSContextRef ctx);
```

`Drain` keeps executing jobs until either:

1. the queue is empty — returns the number of jobs executed, or
2. a job throws — returns the number of successfully executed jobs **before** the throw; writes the first exception to `*exception` and stops.

Typical usage:

```c
xJSValueRef e = NULL;
int ran = xJSContextDrainPendingJobs(ctx, &e);
if (e) {
    // At least one microtask threw and was not caught by a .catch
    // Report it or discard; draining is already halted.
    xJSValueUnprotect(ctx, e);
}
```

### When to call it

Call `xJSContextDrainPendingJobs` whenever host code has performed an action that **may** have scheduled a reaction:

- after calling `resolve()` / `reject()` on a deferred Promise from host land,
- after returning from a host-side async callback that woke JS up,
- before releasing the context if you want `finally` blocks on live Promises to run.

### `xJSAwaitPromise` shortcut

When you already have a specific Promise and want to block until it settles, use [`xJSAwaitPromise()`](module.md#xjsawaitpromise) — it drains internally and returns the fulfilment value (or `NULL` + exception on reject).

## Garbage Collection

```c
void xJSGarbageCollect(xJSContextRef ctx);
```

Forces a full GC on the context's runtime. QuickJS already triggers collection automatically based on allocation pressure; this entry point is useful for:

- **tests** that want deterministic finalizer ordering,
- **idle hooks** in long-running hosts that can afford a pause,
- **leak checks** just before releasing the context.

Only values with zero xjs slot references (i.e. all `xJSValueUnprotect` calls are balanced) and no live JS-side references are reclaimable.

## Best Practices

- **Drain after every host→JS settle.** Whenever host code resolves/rejects a deferred, returns from a native callback that might have woken a Promise, or completes async IO, call `xJSContextDrainPendingJobs`. xjs does not drive an event loop — if you forget, `.then` reactions simply never run.
- **Use `xJSAwaitPromise` for "block until this one settles".** It drains internally and surfaces the fulfilment value or exception; you almost never need a hand-rolled `while (HasPendingJobs) Drain` loop against a specific Promise.
- **Validate with `xJSCheckScriptSyntax` before `xJSEvaluateScript`.** For user-authored scripts (editor, REPL), checking syntax first gives you a clean error channel that cannot also execute side effects.
- **`xJSCheckScriptSyntax` doesn't see module syntax.** Branch on `xJSDetectModule` first; fall through to `xJSEvaluateModule` if the source is a module.
- **Don't leak the throw value.** On a `NULL` return, always `xJSValueUnprotect(ctx, exc)` in the error branch — forgetting is the most common xjs leak.
- **Call `xJSGarbageCollect` sparingly.** QuickJS already collects under pressure; forcing a GC is a multi-ms pause. Reserve it for tests, idle hooks, or pre-shutdown leak checks.

## Caveats

- `startingLineNumber` is currently a no-op on the QuickJS backend; stack-trace line numbers come from source positions alone.
- `xJSCheckScriptSyntax` compiles as a global script — it will **not** catch syntax errors that are only legal in module context (e.g. top-level `import`). Use `xJSDetectModule` first and branch between the two paths if needed (see [module.md](module.md)).
- A runaway script (infinite loop with no job queue interaction) cannot be interrupted from another thread. Host code that embeds untrusted scripts should run them in a dedicated OS thread it can kill.
