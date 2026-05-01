# xjs — Context & Runtime Lifecycle

## Introduction

Every JavaScript operation in xjs happens inside a **global context**, which in turn lives inside a **context group**. The group owns the JS runtime (GC heap, class table, module loader); the context owns the global object and the "value slot" pool used to hand `xJSValueRef` handles back to host code.

Both handles are reference-counted and mirror JavaScriptCore's `JSContextGroupRef` / `JSGlobalContextRef` semantics.

## Object Model

```text
xJSContextGroupRef  (≈ JSRuntime)
  │   - GC heap
  │   - shared class registry
  │   - module loader trampoline
  │
  └── xJSGlobalContextRef  (≈ JSContext, 1..N per group)
        │   - global object
        │   - slot pool for xJSValueRef
        │   - user module-load callback
        │
        └── xJSValueRef / xJSObjectRef / …
```

Most applications only need **one group and one context**; that is what [`xJSGlobalContextCreate(NULL)`](#one-liner-single-context) builds for you.

## Creating and Destroying a Context

### One-liner (single context)

```c
xJSGlobalContextRef ctx = xJSGlobalContextCreate(NULL);
// …
xJSGlobalContextRelease(ctx);
```

`xJSGlobalContextCreate` allocates a fresh group internally, creates one context in it, and transfers group ownership to the context — so `xJSGlobalContextRelease` is the only teardown you need.

### Multiple contexts sharing a heap

```c
xJSContextGroupRef  group = xJSContextGroupCreate();
xJSGlobalContextRef a     = xJSGlobalContextCreateInGroup(group, NULL);
xJSGlobalContextRef b     = xJSGlobalContextCreateInGroup(group, NULL);
// …
xJSGlobalContextRelease(a);
xJSGlobalContextRelease(b);
xJSContextGroupRelease(group);
```

Contexts in the same group share one GC heap — values can be moved between them cheaply — but **must be driven from the same OS thread**. Different groups are fully independent and may run on different threads.

### Naming a context (for stack traces)

```c
xJSStringRef name = xJSStringCreateWithUTF8CString("worker-42");
xJSGlobalContextSetName(ctx, name);
xJSStringRelease(name);
```

The name shows up in QuickJS error messages and makes multi-context deployments easier to debug.

## Accessing the Global Object

```c
xJSObjectRef g = xJSContextGetGlobalObject(ctx);
// install globals on `g` via xJSObjectSetProperty
xJSValueUnprotect(ctx, (xJSValueRef)g);  // release our reference
```

`xJSContextGetGlobalObject` returns a **new** reference (as do all `Get*` helpers in xjs; see [value.md](value.md) for the lifetime rules).

## Pumping Microtasks

QuickJS does not execute Promise reactions automatically between host invocations. Whenever host code does something that might settle a Promise (resolve a deferred, return from a native callback, complete an IO operation), call:

```c
xJSValueRef exc = NULL;
int ran = xJSContextDrainPendingJobs(ctx, &exc);
if (ran < 0 && exc) {
    // first job to throw; subsequent jobs still queued
}
```

The helper keeps executing pending jobs until the queue is empty or a job throws. `xJSContextHasPendingJobs()` is a cheap peek when you want to batch-drain only when needed.

For the common "evaluate a module, block until done" flow, use [`xJSAwaitPromise()`](module.md) instead — it drains on your behalf until a specific Promise settles.

## Installing a Module Loader

```c
xJSContextSetModuleLoader(ctx, my_loader, my_opaque);
```

See [module.md](module.md) for the loader contract (it is always installed internally; passing `NULL` just reverts to the built-in ReferenceError behaviour for every `import`).

## API Surface

### Context group

```c
xJSContextGroupRef xJSContextGroupCreate(void);
xJSContextGroupRef xJSContextGroupRetain(xJSContextGroupRef group);
void               xJSContextGroupRelease(xJSContextGroupRef group);
```

### Global context

```c
xJSGlobalContextRef xJSGlobalContextCreate(xJSClassRef globalObjectClass);
xJSGlobalContextRef xJSGlobalContextCreateInGroup(xJSContextGroupRef group,
                                                  xJSClassRef globalObjectClass);
xJSGlobalContextRef xJSGlobalContextRetain(xJSGlobalContextRef ctx);
void                xJSGlobalContextRelease(xJSGlobalContextRef ctx);

xJSStringRef xJSGlobalContextCopyName(xJSGlobalContextRef ctx);
void         xJSGlobalContextSetName(xJSGlobalContextRef ctx, xJSStringRef name);

xJSObjectRef        xJSContextGetGlobalObject(xJSContextRef ctx);
xJSContextGroupRef  xJSContextGetGroup(xJSContextRef ctx);
xJSGlobalContextRef xJSContextGetGlobalContext(xJSContextRef ctx);
```

### Microtask pump

```c
int  xJSContextDrainPendingJobs(xJSContextRef ctx, xJSValueRef *exception);
bool xJSContextHasPendingJobs(xJSContextRef ctx);
```

### Module loader

```c
typedef xJSStringRef (*xJSModuleLoadCallback)(xJSContextRef ctx,
                                              const char   *normalizedName,
                                              void         *opaque);
void xJSContextSetModuleLoader(xJSGlobalContextRef   ctx,
                               xJSModuleLoadCallback load, void *opaque);
```

## Caveats

- `xJSGlobalContextCreate(xJSClassRef globalObjectClass)` currently **ignores** `globalObjectClass`: customising the global object type is on the roadmap but not yet wired through. Pass `NULL`.
- Contexts are **not thread-safe** — every entry into `ctx` (including `xJSValueUnprotect`) must come from the thread that owns the group.
