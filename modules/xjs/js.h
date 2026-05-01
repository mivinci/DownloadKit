/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js.h - JavaScript scripting engine public API
 *
 * xjs is a lightweight JavaScript engine designed to be embedded in
 * xKit applications.  It is currently implemented on top of QuickJS,
 * but the public surface intentionally mirrors Apple's JavaScriptCore
 * C API (https://developer.apple.com/documentation/javascriptcore)
 * so the engine backend can be swapped without touching callers.
 *
 * Naming conventions follow JSC verbatim, with a single global
 * substitution "JS" -> "xJS" (and "kJS" -> "kXJS", "OpaqueJS" ->
 * "OpaqueXJS").  The XJS_ prefix is used for header guards and
 * a handful of xKit-specific extensions documented below.
 *
 * Deliberate deviations from JSC:
 *   - QuickJS internal types (JSValue, JSRuntime, ...) are never
 *     exposed here.
 *   - xjs itself does not drive an event loop.  Host code is
 *     responsible for pumping microtasks / async jobs by calling
 *     xJSContextDrainPendingJobs() at the appropriate moment
 *     (typically after returning from JS into host code, or when
 *     a host-side callback resolves a promise via JS_EnqueueJob).
 */

#ifndef XJS_JS_H
#define XJS_JS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <xbase/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Opaque reference types
 *
 * These mirror the JSC types one-to-one.  We do NOT use xKit's
 * XDEF_HANDLE macro here because we want strong typing between the
 * different Ref kinds (e.g. passing an xJSStringRef where an
 * xJSValueRef is expected must be a compile-time error).
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct OpaqueXJSContextGroup            *xJSContextGroupRef;
typedef const struct OpaqueXJSContext           *xJSContextRef;
typedef struct OpaqueXJSContext                 *xJSGlobalContextRef;
typedef const struct OpaqueXJSValue             *xJSValueRef;
typedef struct OpaqueXJSValue                   *xJSObjectRef;
typedef struct OpaqueXJSString                  *xJSStringRef;
typedef struct OpaqueXJSClass                   *xJSClassRef;
typedef struct OpaqueXJSPropertyNameArray       *xJSPropertyNameArrayRef;
typedef struct OpaqueXJSPropertyNameAccumulator *xJSPropertyNameAccumulatorRef;

/* ═══════════════════════════════════════════════════════════════════
 * Enumerations
 * ═══════════════════════════════════════════════════════════════════ */

/** JavaScript runtime type of a value. */
typedef enum {
  kXJSTypeUndefined = 0,
  kXJSTypeNull      = 1,
  kXJSTypeBoolean   = 2,
  kXJSTypeNumber    = 3,
  kXJSTypeString    = 4,
  kXJSTypeObject    = 5,
  kXJSTypeSymbol    = 6,
} xJSType;

/** Type-class of an object (primarily useful for Typed Arrays). */
typedef enum {
  kXJSTypedArrayTypeNone              = 0,
  kXJSTypedArrayTypeInt8Array         = 1,
  kXJSTypedArrayTypeInt16Array        = 2,
  kXJSTypedArrayTypeInt32Array        = 3,
  kXJSTypedArrayTypeUint8Array        = 4,
  kXJSTypedArrayTypeUint8ClampedArray = 5,
  kXJSTypedArrayTypeUint16Array       = 6,
  kXJSTypedArrayTypeUint32Array       = 7,
  kXJSTypedArrayTypeFloat32Array      = 8,
  kXJSTypedArrayTypeFloat64Array      = 9,
  kXJSTypedArrayTypeBigInt64Array     = 10,
  kXJSTypedArrayTypeBigUint64Array    = 11,
  kXJSTypedArrayTypeArrayBuffer       = 12,
} xJSTypedArrayType;

/** Attribute flags for object properties (bit-ORed). */
typedef unsigned xJSPropertyAttributes;
enum {
  kXJSPropertyAttributeNone       = 0,
  kXJSPropertyAttributeReadOnly   = 1 << 1,
  kXJSPropertyAttributeDontEnum   = 1 << 2,
  kXJSPropertyAttributeDontDelete = 1 << 3,
};

/** Class-definition attribute flags. */
typedef unsigned xJSClassAttributes;
enum {
  kXJSClassAttributeNone                 = 0,
  kXJSClassAttributeNoAutomaticPrototype = 1 << 1,
};

/* ═══════════════════════════════════════════════════════════════════
 * Class callbacks  (prototypes needed before xJSClassDefinition)
 * ═══════════════════════════════════════════════════════════════════ */

typedef void (*xJSObjectInitializeCallback)(xJSContextRef ctx,
                                            xJSObjectRef  object);

typedef void (*xJSObjectFinalizeCallback)(xJSObjectRef object);

typedef bool (*xJSObjectHasPropertyCallback)(xJSContextRef ctx,
                                             xJSObjectRef  object,
                                             xJSStringRef  propertyName);

typedef xJSValueRef (*xJSObjectGetPropertyCallback)(xJSContextRef ctx,
                                                    xJSObjectRef  object,
                                                    xJSStringRef  propertyName,
                                                    xJSValueRef  *exception);

typedef bool (*xJSObjectSetPropertyCallback)(xJSContextRef ctx,
                                             xJSObjectRef  object,
                                             xJSStringRef  propertyName,
                                             xJSValueRef   value,
                                             xJSValueRef  *exception);

typedef bool (*xJSObjectDeletePropertyCallback)(xJSContextRef ctx,
                                                xJSObjectRef  object,
                                                xJSStringRef  propertyName,
                                                xJSValueRef  *exception);

typedef void (*xJSObjectGetPropertyNamesCallback)(
  xJSContextRef ctx, xJSObjectRef object,
  xJSPropertyNameAccumulatorRef propertyNames);

typedef xJSValueRef (*xJSObjectCallAsFunctionCallback)(
  xJSContextRef ctx, xJSObjectRef function, xJSObjectRef thisObject,
  size_t argumentCount, const xJSValueRef arguments[], xJSValueRef *exception);

typedef xJSObjectRef (*xJSObjectCallAsConstructorCallback)(
  xJSContextRef ctx, xJSObjectRef constructor, size_t argumentCount,
  const xJSValueRef arguments[], xJSValueRef *exception);

typedef bool (*xJSObjectHasInstanceCallback)(xJSContextRef ctx,
                                             xJSObjectRef  constructor,
                                             xJSValueRef   possibleInstance,
                                             xJSValueRef  *exception);

typedef xJSValueRef (*xJSObjectConvertToTypeCallback)(xJSContextRef ctx,
                                                      xJSObjectRef  object,
                                                      xJSType       type,
                                                      xJSValueRef  *exception);

/** Static function entry for xJSClassDefinition::staticFunctions. */
XDEF_STRUCT(xJSStaticFunction) {
  const char                     *name;
  xJSObjectCallAsFunctionCallback callAsFunction;
  xJSPropertyAttributes           attributes;
};

/** Static value entry for xJSClassDefinition::staticValues. */
XDEF_STRUCT(xJSStaticValue) {
  const char                  *name;
  xJSObjectGetPropertyCallback getProperty;
  xJSObjectSetPropertyCallback setProperty;
  xJSPropertyAttributes        attributes;
};

/**
 * Class definition.  Fully source-compatible layout with JSC's
 * JSClassDefinition — the two structs can be used almost
 * interchangeably at the field level.
 */
XDEF_STRUCT(xJSClassDefinition) {
  int                version; /* must be 0  */
  xJSClassAttributes attributes;
  const char        *className;
  xJSClassRef        parentClass;

  const xJSStaticValue    *staticValues;
  const xJSStaticFunction *staticFunctions;

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

/**
 * All-zero class definition, convenient to copy-initialize:
 *     xJSClassDefinition def = kXJSClassDefinitionEmpty;
 *     def.className = "Foo";
 *     def.finalize  = foo_finalize;
 */
XCAPI(const xJSClassDefinition) kXJSClassDefinitionEmpty;

/* ═══════════════════════════════════════════════════════════════════
 * Context group / global context  (≈ runtime & context)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Create a new context group (shared heap).  Mirrors
 * JSContextGroupCreate().
 */
XCAPI(xJSContextGroupRef) xJSContextGroupCreate(void);

XCAPI(xJSContextGroupRef) xJSContextGroupRetain(xJSContextGroupRef group);
XCAPI(void)               xJSContextGroupRelease(xJSContextGroupRef group);

/**
 * Create a global execution context in a new (auto-allocated) group.
 * Pass NULL for @p globalObjectClass to use the default global object.
 */
XCAPI(xJSGlobalContextRef)
xJSGlobalContextCreate(xJSClassRef globalObjectClass);

/** Create a global execution context in the given group. */
XCAPI(xJSGlobalContextRef)
xJSGlobalContextCreateInGroup(xJSContextGroupRef group,
                              xJSClassRef        globalObjectClass);

XCAPI(xJSGlobalContextRef) xJSGlobalContextRetain(xJSGlobalContextRef ctx);
XCAPI(void)                xJSGlobalContextRelease(xJSGlobalContextRef ctx);

XCAPI(xJSObjectRef)       xJSContextGetGlobalObject(xJSContextRef ctx);
XCAPI(xJSContextGroupRef) xJSContextGetGroup(xJSContextRef ctx);
XCAPI(xJSGlobalContextRef)
xJSContextGetGlobalContext(xJSContextRef ctx);

XCAPI(xJSStringRef) xJSGlobalContextCopyName(xJSGlobalContextRef ctx);
XCAPI(void) xJSGlobalContextSetName(xJSGlobalContextRef ctx, xJSStringRef name);

/* ═══════════════════════════════════════════════════════════════════
 * Value: type queries & construction
 * ═══════════════════════════════════════════════════════════════════ */

XCAPI(xJSType) xJSValueGetType(xJSContextRef ctx, xJSValueRef value);

XCAPI(bool) xJSValueIsUndefined(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsNull(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsBoolean(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsNumber(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsString(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsSymbol(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsObject(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsObjectOfClass(xJSContextRef ctx, xJSValueRef value,
                                    xJSClassRef jsClass);
XCAPI(bool) xJSValueIsArray(xJSContextRef ctx, xJSValueRef value);
XCAPI(bool) xJSValueIsDate(xJSContextRef ctx, xJSValueRef value);

XCAPI(bool) xJSValueIsEqual(xJSContextRef ctx, xJSValueRef a, xJSValueRef b,
                            xJSValueRef *exception);
XCAPI(bool) xJSValueIsStrictEqual(xJSContextRef ctx, xJSValueRef a,
                                  xJSValueRef b);
XCAPI(bool) xJSValueIsInstanceOfConstructor(xJSContextRef ctx,
                                            xJSValueRef   value,
                                            xJSObjectRef  constructor,
                                            xJSValueRef  *exception);

XCAPI(xJSValueRef) xJSValueMakeUndefined(xJSContextRef ctx);
XCAPI(xJSValueRef) xJSValueMakeNull(xJSContextRef ctx);
XCAPI(xJSValueRef) xJSValueMakeBoolean(xJSContextRef ctx, bool value);
XCAPI(xJSValueRef) xJSValueMakeNumber(xJSContextRef ctx, double value);
XCAPI(xJSValueRef) xJSValueMakeString(xJSContextRef ctx, xJSStringRef s);
XCAPI(xJSValueRef) xJSValueMakeSymbol(xJSContextRef ctx,
                                      xJSStringRef  description);

/* JSON bridge. */
XCAPI(xJSValueRef)  xJSValueMakeFromJSONString(xJSContextRef ctx,
                                               xJSStringRef  json);
XCAPI(xJSStringRef) xJSValueCreateJSONString(xJSContextRef ctx,
                                             xJSValueRef value, unsigned indent,
                                             xJSValueRef *exception);

/* Conversions.  "Copy" in the name means the caller owns the result. */
XCAPI(bool)         xJSValueToBoolean(xJSContextRef ctx, xJSValueRef value);
XCAPI(double)       xJSValueToNumber(xJSContextRef ctx, xJSValueRef value,
                                     xJSValueRef *exception);
XCAPI(xJSStringRef) xJSValueToStringCopy(xJSContextRef ctx, xJSValueRef value,
                                         xJSValueRef *exception);
XCAPI(xJSObjectRef) xJSValueToObject(xJSContextRef ctx, xJSValueRef value,
                                     xJSValueRef *exception);

/* GC rooting (retain JS values across JS API boundaries). */
XCAPI(void) xJSValueProtect(xJSContextRef ctx, xJSValueRef value);
XCAPI(void) xJSValueUnprotect(xJSContextRef ctx, xJSValueRef value);

/* ═══════════════════════════════════════════════════════════════════
 * Object
 * ═══════════════════════════════════════════════════════════════════ */

XCAPI(xJSObjectRef)
xJSObjectMake(xJSContextRef ctx, xJSClassRef jsClass, void *data);

XCAPI(xJSObjectRef)
xJSObjectMakeFunctionWithCallback(
  xJSContextRef ctx, xJSStringRef name,
  xJSObjectCallAsFunctionCallback callAsFunction);

XCAPI(xJSObjectRef)
xJSObjectMakeConstructor(xJSContextRef ctx, xJSClassRef jsClass,
                         xJSObjectCallAsConstructorCallback callAsConstructor);

XCAPI(xJSObjectRef)
xJSObjectMakeArray(xJSContextRef ctx, size_t argumentCount,
                   const xJSValueRef arguments[], xJSValueRef *exception);

XCAPI(xJSObjectRef)
xJSObjectMakeDate(xJSContextRef ctx, size_t argumentCount,
                  const xJSValueRef arguments[], xJSValueRef *exception);

XCAPI(xJSObjectRef)
xJSObjectMakeError(xJSContextRef ctx, size_t argumentCount,
                   const xJSValueRef arguments[], xJSValueRef *exception);

XCAPI(xJSObjectRef)
xJSObjectMakeRegExp(xJSContextRef ctx, size_t argumentCount,
                    const xJSValueRef arguments[], xJSValueRef *exception);

XCAPI(xJSObjectRef)
xJSObjectMakeDeferredPromise(xJSContextRef ctx, xJSObjectRef *resolve,
                             xJSObjectRef *reject, xJSValueRef *exception);

/* Compile-and-return-function (a la `new Function(...)`). */
XCAPI(xJSObjectRef)
xJSObjectMakeFunction(xJSContextRef ctx, xJSStringRef name,
                      unsigned           parameterCount,
                      const xJSStringRef parameterNames[], xJSStringRef body,
                      xJSStringRef sourceURL, int startingLineNumber,
                      xJSValueRef *exception);

XCAPI(xJSValueRef) xJSObjectGetPrototype(xJSContextRef ctx,
                                         xJSObjectRef  object);
XCAPI(void)        xJSObjectSetPrototype(xJSContextRef ctx, xJSObjectRef object,
                                         xJSValueRef value);

XCAPI(bool)        xJSObjectHasProperty(xJSContextRef ctx, xJSObjectRef object,
                                        xJSStringRef propertyName);
XCAPI(xJSValueRef) xJSObjectGetProperty(xJSContextRef ctx, xJSObjectRef object,
                                        xJSStringRef propertyName,
                                        xJSValueRef *exception);
XCAPI(void)        xJSObjectSetProperty(xJSContextRef ctx, xJSObjectRef object,
                                        xJSStringRef propertyName, xJSValueRef value,
                                        xJSPropertyAttributes attributes,
                                        xJSValueRef          *exception);
XCAPI(bool) xJSObjectDeleteProperty(xJSContextRef ctx, xJSObjectRef object,
                                    xJSStringRef propertyName,
                                    xJSValueRef *exception);

XCAPI(xJSValueRef) xJSObjectGetPropertyAtIndex(xJSContextRef ctx,
                                               xJSObjectRef  object,
                                               unsigned      propertyIndex,
                                               xJSValueRef  *exception);
XCAPI(void) xJSObjectSetPropertyAtIndex(xJSContextRef ctx, xJSObjectRef object,
                                        unsigned     propertyIndex,
                                        xJSValueRef  value,
                                        xJSValueRef *exception);

XCAPI(void *) xJSObjectGetPrivate(xJSObjectRef object);
XCAPI(bool)   xJSObjectSetPrivate(xJSObjectRef object, void *data);

XCAPI(bool) xJSObjectIsFunction(xJSContextRef ctx, xJSObjectRef object);
XCAPI(xJSValueRef)
xJSObjectCallAsFunction(xJSContextRef ctx, xJSObjectRef object,
                        xJSObjectRef thisObject, size_t argumentCount,
                        const xJSValueRef arguments[], xJSValueRef *exception);

XCAPI(bool) xJSObjectIsConstructor(xJSContextRef ctx, xJSObjectRef object);
XCAPI(xJSObjectRef) xJSObjectCallAsConstructor(xJSContextRef     ctx,
                                               xJSObjectRef      object,
                                               size_t            argumentCount,
                                               const xJSValueRef arguments[],
                                               xJSValueRef      *exception);

XCAPI(xJSPropertyNameArrayRef)
xJSObjectCopyPropertyNames(xJSContextRef ctx, xJSObjectRef object);

XCAPI(xJSPropertyNameArrayRef)
xJSPropertyNameArrayRetain(xJSPropertyNameArrayRef array);
XCAPI(void) xJSPropertyNameArrayRelease(xJSPropertyNameArrayRef array);
XCAPI(size_t)
xJSPropertyNameArrayGetCount(xJSPropertyNameArrayRef array);
XCAPI(xJSStringRef)
xJSPropertyNameArrayGetNameAtIndex(xJSPropertyNameArrayRef array, size_t index);

XCAPI(void)
xJSPropertyNameAccumulatorAddName(xJSPropertyNameAccumulatorRef accumulator,
                                  xJSStringRef                  propertyName);

/* ═══════════════════════════════════════════════════════════════════
 * Class registration
 * ═══════════════════════════════════════════════════════════════════ */

XCAPI(xJSClassRef) xJSClassCreate(const xJSClassDefinition *definition);
XCAPI(xJSClassRef) xJSClassRetain(xJSClassRef jsClass);
XCAPI(void)        xJSClassRelease(xJSClassRef jsClass);

/* ═══════════════════════════════════════════════════════════════════
 * String
 *
 * xJSStringRef owns its data; internally stored as UTF-16.  The
 * *UTF8* helpers transcode on the fly.
 * ═══════════════════════════════════════════════════════════════════ */

XCAPI(xJSStringRef) xJSStringCreateWithCharacters(const uint16_t *chars,
                                                  size_t          numChars);
XCAPI(xJSStringRef) xJSStringCreateWithUTF8CString(const char *string);

XCAPI(xJSStringRef) xJSStringRetain(xJSStringRef string);
XCAPI(void)         xJSStringRelease(xJSStringRef string);

XCAPI(size_t)           xJSStringGetLength(xJSStringRef string);
XCAPI(const uint16_t *) xJSStringGetCharactersPtr(xJSStringRef string);
XCAPI(size_t)           xJSStringGetMaximumUTF8CStringSize(xJSStringRef string);
XCAPI(size_t) xJSStringGetUTF8CString(xJSStringRef string, char *buffer,
                                      size_t bufferSize);

XCAPI(bool) xJSStringIsEqual(xJSStringRef a, xJSStringRef b);
XCAPI(bool) xJSStringIsEqualToUTF8CString(xJSStringRef a, const char *b);

/* ═══════════════════════════════════════════════════════════════════
 * Script evaluation
 * ═══════════════════════════════════════════════════════════════════ */

XCAPI(bool)
xJSCheckScriptSyntax(xJSContextRef ctx, xJSStringRef script,
                     xJSStringRef sourceURL, int startingLineNumber,
                     xJSValueRef *exception);

XCAPI(xJSValueRef)
xJSEvaluateScript(xJSContextRef ctx, xJSStringRef script,
                  xJSObjectRef thisObject, xJSStringRef sourceURL,
                  int startingLineNumber, xJSValueRef *exception);

XCAPI(void) xJSGarbageCollect(xJSContextRef ctx);

/*
 * Drain all currently-pending microtasks (Promise reactions, async/
 * await continuations, queueMicrotask jobs …) on the runtime that
 * owns `ctx`.  QuickJS does not automatically flush its job queue
 * between host invocations — call this once host-side mutators
 * (e.g. calling a deferred-promise resolver) need their observable
 * side-effects visible to subsequent JS or host code.
 *
 * Returns the number of jobs executed successfully.  If any job
 * threw, the first exception encountered is written to *exception
 * (if non-NULL) and draining stops; return value is the number of
 * successful jobs before the failure.  Returns 0 if `ctx` is null
 * or if there were no pending jobs.
 */
XCAPI(int)
xJSContextDrainPendingJobs(xJSContextRef ctx, xJSValueRef *exception);

/*
 * Non-intrusive peek — true iff the runtime has at least one job
 * queued.  Convenient for embedders that want to batch-drain only
 * when needed.
 */
XCAPI(bool) xJSContextHasPendingJobs(xJSContextRef ctx);

/* ═══════════════════════════════════════════════════════════════════
 * ES modules  (xKit extension)
 *
 * JavaScriptCore exposes module support only through its private
 * Objective-C API (JSScript + JSModuleLoaderDelegate), not the C
 * API we otherwise mirror.  This section is therefore xjs-specific
 * but the shape is inspired by JSC's design:
 *
 *   - A module identifier is a string; the same string is used as
 *     sourceURL when compiling the module.  Specifier normalisation
 *     (resolving "./x" relative to the importer) is handled
 *     internally — the load callback only sees normalised names.
 *
 *   - Evaluating a module is asynchronous by construction: it
 *     returns a Promise that fulfils to the module namespace once
 *     all transitive imports have loaded and executed.  Use
 *     xJSAwaitPromise() to block the calling thread until the
 *     Promise settles (draining microtasks along the way).
 *
 *   - Native modules (i.e. host code registered as JS modules) are
 *     intentionally not supported in this first cut.  Expose host
 *     functionality via the global object instead.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Heuristic: does @p source read like an ES module rather than a
 * classic script?  Mirrors QuickJS's JS_DetectModule — detects
 * top-level `import`/`export` tokens.  A cheap pre-pass to let
 * the embedder pick between xJSEvaluateScript and xJSEvaluateModule.
 */
XCAPI(bool) xJSDetectModule(const char *source, size_t length);

/**
 * Compile and start evaluating an ES module.  Returns a Promise
 * that fulfils to the module namespace object, or rejects if any
 * import or top-level statement throws.
 *
 * @param ctx         Execution context.
 * @param script      Module source code.  Must not be NULL.
 * @param sourceURL   Module identifier.  Used both as the compile-
 *                    time sourceURL (for stack traces and
 *                    `import.meta.url`) and as the base specifier
 *                    against which relative imports in @p script
 *                    are normalised.  Pass NULL for an anonymous
 *                    entry point ("<xjs>").
 * @param exception   Out-param for compile-time errors only.  A
 *                    *runtime* error (rejection) surfaces through
 *                    the returned Promise.
 *
 * @return A Promise value on success; NULL on compile/setup error
 *         (in which case @p exception is populated if non-NULL).
 */
XCAPI(xJSValueRef)
xJSEvaluateModule(xJSContextRef ctx, xJSStringRef script,
                  xJSStringRef sourceURL, xJSValueRef *exception);

/**
 * Synchronously drain pending jobs on @p ctx until @p promise
 * settles.  Returns the fulfilment value on resolve; returns NULL
 * and populates @p exception on reject.
 *
 * If @p promise is not actually a Promise it is returned as-is
 * (no drain); this makes the helper safe to wrap around the result
 * of xJSEvaluateModule even in the hypothetical case where QuickJS
 * returns an already-settled value.
 *
 * The caller retains ownership of @p promise and is responsible
 * for releasing the returned value (if any).
 */
XCAPI(xJSValueRef)
xJSAwaitPromise(xJSContextRef ctx, xJSValueRef promise, xJSValueRef *exception);

/**
 * Module loader callback.  Invoked the first time a given
 * normalised module name is requested; xjs caches the result of
 * each successful compile so this is called at most once per
 * identifier per context.
 *
 * Return the module source as a freshly-created xJSStringRef; xjs
 * takes ownership (Release after use).  Returning NULL signals
 * "module not found" and causes the importing evaluation to
 * reject with a ReferenceError.
 */
typedef xJSStringRef (*xJSModuleLoadCallback)(xJSContextRef ctx,
                                              const char   *normalizedName,
                                              void         *opaque);

/**
 * Install (or clear, by passing NULL) the module loader on @p ctx.
 * Only one loader may be active at a time.  @p opaque is passed
 * unchanged to every invocation of @p load.
 */
XCAPI(void) xJSContextSetModuleLoader(xJSGlobalContextRef   ctx,
                                      xJSModuleLoadCallback load, void *opaque);

#ifdef __cplusplus
}
#endif

#endif /* XJS_JS_H */
