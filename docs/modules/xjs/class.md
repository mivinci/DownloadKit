# xjs — Classes & Native Wrappers

## Introduction

A **class** in xjs is a recipe for wrapping a C struct as a JavaScript object — the same role `JSClassRef` plays in JavaScriptCore. A class ties together:

- a **class name** (shows up in `Object.prototype.toString`),
- a **finalizer** that runs when the wrapped instance is garbage-collected,
- optional **property callbacks** (`hasProperty` / `getProperty` / `setProperty` / `deleteProperty` / `getPropertyNames`) for exotic access patterns,
- optional **call / construct / hasInstance / convertToType** hooks,
- static **value** and **function** tables installed on the prototype,
- an **initializer** invoked when new instances are created.

## The Definition Struct

```c
XDEF_STRUCT(xJSClassDefinition) {
  int                version;     /* must be 0 */
  xJSClassAttributes attributes;  /* bitmask */
  const char        *className;
  xJSClassRef        parentClass;

  const xJSStaticValue    *staticValues;    /* NULL-terminated */
  const xJSStaticFunction *staticFunctions; /* NULL-terminated */

  xJSObjectInitializeCallback        initialize;
  xJSObjectFinalizeCallback          finalize;
  xJSObjectHasPropertyCallback       hasProperty;
  xJSObjectGetPropertyCallback       getProperty;
  xJSObjectSetPropertyCallback       setProperty;
  xJSObjectDeletePropertyCallback    deleteProperty;
  xJSObjectGetPropertyNamesCallback  getPropertyNames;
  xJSObjectCallAsFunctionCallback    callAsFunction;
  xJSObjectCallAsConstructorCallback callAsConstructor;
  xJSObjectHasInstanceCallback       hasInstance;
  xJSObjectConvertToTypeCallback     convertToType;
};
```

Layout matches JSC's `JSClassDefinition` field-for-field. A zero-initialised helper is provided:

```c
xJSClassDefinition def = kXJSClassDefinitionEmpty;
def.className = "Counter";
def.finalize  = counter_finalize;
```

## Lifecycle

```c
xJSClassRef xJSClassCreate(const xJSClassDefinition *def);
xJSClassRef xJSClassRetain(xJSClassRef cls);
void        xJSClassRelease(xJSClassRef cls);
```

`xJSClassCreate` is runtime-agnostic — it does **not** need an `xJSContextRef`. Callers typically build classes at module-init time and keep them in globals for the lifetime of the process. The first time an instance of the class is created or tested against (via `xJSObjectMake`, `xJSObjectMakeConstructor`, `xJSValueIsObjectOfClass`), xjs lazily registers the class against the context's runtime; subsequent uses on the same runtime are no-ops.

The same `xJSClassRef` can be shared across multiple runtimes in the same process — each runtime registers it once and allocates its own class-ID table.

## Finalizer Contract

```c
typedef void (*xJSObjectFinalizeCallback)(xJSObjectRef object);
```

Important constraints:

- **Runs during GC**, so the wrapped `xJSContextRef` is **not available** — passing `object` to APIs that require a live context (anything that evaluates code, reads properties via scripted accessors, …) is undefined behaviour.
- **Safe operations**: `xJSObjectGetPrivate(object)` to retrieve the `void *` you stored at `xJSObjectMake` time, so you can free it.
- Finalizers may run in any order relative to other finalizers — do not rely on ordering between instances of different classes.

## Full Example — a native Counter

```c
typedef struct { long value; } Counter;

static void counter_finalize(xJSObjectRef obj) {
    free(xJSObjectGetPrivate(obj));
}

static xJSValueRef counter_inc(xJSContextRef ctx, xJSObjectRef fn,
                               xJSObjectRef thiz, size_t argc,
                               const xJSValueRef argv[], xJSValueRef *exc) {
    (void)fn; (void)argc; (void)argv; (void)exc;
    Counter *c = (Counter *)xJSObjectGetPrivate(thiz);
    c->value++;
    return xJSValueMakeUndefined(ctx);
}

static xJSValueRef counter_get(xJSContextRef ctx, xJSObjectRef thiz,
                               xJSStringRef name, xJSValueRef *exc) {
    (void)name; (void)exc;
    Counter *c = (Counter *)xJSObjectGetPrivate(thiz);
    return xJSValueMakeNumber(ctx, (double)c->value);
}

static xJSObjectRef counter_construct(xJSContextRef ctx, xJSObjectRef ctor,
                                      size_t argc, const xJSValueRef argv[],
                                      xJSValueRef *exc) {
    (void)ctor; (void)argc; (void)argv; (void)exc;
    Counter *c = calloc(1, sizeof(*c));
    return xJSObjectMake(ctx, s_counter_class, c);   // see below
}

static const xJSStaticFunction kFns[] = {
    { "inc", counter_inc, kXJSPropertyAttributeDontDelete },
    { NULL, NULL, 0 },
};
static const xJSStaticValue kVals[] = {
    { "value", counter_get, NULL, kXJSPropertyAttributeDontDelete | kXJSPropertyAttributeReadOnly },
    { NULL, NULL, NULL, 0 },
};

xJSClassRef s_counter_class;

void register_counter(xJSGlobalContextRef ctx) {
    xJSClassDefinition def = kXJSClassDefinitionEmpty;
    def.className       = "Counter";
    def.finalize        = counter_finalize;
    def.staticFunctions = kFns;
    def.staticValues    = kVals;
    s_counter_class = xJSClassCreate(&def);

    xJSObjectRef ctor = xJSObjectMakeConstructor(ctx, s_counter_class, counter_construct);
    xJSStringRef name = xJSStringCreateWithUTF8CString("Counter");
    xJSObjectRef g    = xJSContextGetGlobalObject(ctx);
    xJSObjectSetProperty(ctx, g, name, (xJSValueRef)ctor, 0, NULL);
    xJSStringRelease(name);
    xJSValueUnprotect(ctx, (xJSValueRef)g);
    xJSValueUnprotect(ctx, (xJSValueRef)ctor);
}
```

Now JS code can do:

```js
const c = new Counter();
c.inc(); c.inc(); c.inc();
console.log(c.value); // 3
```

## Class Attributes

```c
kXJSClassAttributeNone                 = 0
kXJSClassAttributeNoAutomaticPrototype = 1 << 1
```

`NoAutomaticPrototype` suppresses the auto-wired prototype chain — use it when `parentClass` is set and you need exact control over prototype linking.

## Static Tables

### `xJSStaticValue`

```c
XDEF_STRUCT(xJSStaticValue) {
    const char                  *name;
    xJSObjectGetPropertyCallback getProperty;
    xJSObjectSetPropertyCallback setProperty;
    xJSPropertyAttributes        attributes;
};
```

A NULL-terminated array installs one accessor per entry on the class prototype. Omit `setProperty` for a read-only property (pair it with `kXJSPropertyAttributeReadOnly` to keep the flag consistent).

### `xJSStaticFunction`

```c
XDEF_STRUCT(xJSStaticFunction) {
    const char                     *name;
    xJSObjectCallAsFunctionCallback callAsFunction;
    xJSPropertyAttributes           attributes;
};
```

Also NULL-terminated. Each entry becomes a prototype method.

## Best Practices

- **Build classes once, reuse forever.** `xJSClassCreate` is runtime-agnostic and the resulting `xJSClassRef` can be shared across every context/group in the process. Stash it in a `static` global at init time.
- **Keep static tables `static const`.** The class only shallow-copies the definition, so the `staticValues` / `staticFunctions` arrays and the `className` string must outlive the class. `static const` arrays satisfy this for free.
- **Free private data in `finalize`, nowhere else.** It is the only callback guaranteed to run exactly once per instance. Do not rely on explicit teardown from host code — the object may still be alive when the context is released.
- **Don't touch the context inside `finalize`.** The finalizer runs during GC with no live context; limit yourself to `xJSObjectGetPrivate` + `free` (or equivalent).
- **Prefer `xJSStaticFunction` / `xJSStaticValue` over per-instance property installs.** Static tables attach to the prototype once and cost nothing per instance; installing properties in `initialize` multiplies memory and GC work by the instance count.

## Caveats

- `xJSClassCreate` only makes a shallow copy of the definition — `staticValues`, `staticFunctions` and `className` pointers must stay alive for the class's lifetime (use `static const` tables as in the example).
- The class holds **no retain** on `parentClass`; you must keep it alive yourself.
- Private data is a single `void *`. For structured data, define a struct and store a pointer to it. xjs never touches the pointer other than to hand it back from `xJSObjectGetPrivate`.
- `hasInstance` and `convertToType` callbacks are accepted in the definition for JSC parity but are **not yet wired** to QuickJS semantics. Avoid depending on them until the backend grows matching hooks.
