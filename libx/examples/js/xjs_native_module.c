/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xjs_native_module.c -- "native module" demo.
 *
 * xjs deliberately does *not* offer an API for registering a
 * JSModuleDef that's backed by C functions (see the js.h
 * "ES modules" section for the rationale: JSC doesn't expose one
 * either, and the extra API surface/lifetime complexity isn't worth
 * it for a first cut).  Instead, the recommended pattern is the
 * same one used in embedded JSC:
 *
 *   1. Register native functions on the global object under a
 *      well-known (double-underscored, non-enumerable-in-spirit)
 *      key.  This is what the JS runtime actually calls.
 *   2. In the module loader, synthesise a thin JS facade module
 *      whenever the user `import`s a known bare specifier.  The
 *      facade just re-exports the relevant slots of the global
 *      hook under ergonomic names.
 *
 * End result from the user's side:
 *
 *     import { increment, get, reset } from "counter";
 *     import { log } from "console";
 *
 *     for (let i = 0; i < 3; i++) increment();
 *     log("count =", get());   // count = 3
 *
 * ...which is what you'd get from a proper native module, with
 * zero coupling of the JS code to the underlying hook convention.
 *
 * Build: target `xjs_native_module` (added in examples/CMakeLists.txt).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x/js/js.h>

/* ───────────────────── host state ────────────────────────────────
 *
 * A singleton counter, shared across all `import "counter"` calls
 * (because there's only one JSContext in this demo).  In real code
 * you'd keep the state inside an opaque passed to the callback,
 * but globals are fine for a 1-file example.
 */
static long g_counter = 0;

/* ───────────────────── native callbacks ─────────────────────────
 *
 * Signature: `xJSObjectCallAsFunctionCallback`.  We ignore `function`
 * and `thisObject`; the module facade invokes us as plain functions.
 */

static xJSValueRef native_counter_inc(xJSContextRef ctx, xJSObjectRef fn,
                                      xJSObjectRef this_obj, size_t argc,
                                      const xJSValueRef argv[],
                                      xJSValueRef      *exc) {
  (void)fn;
  (void)this_obj;
  (void)argc;
  (void)argv;
  (void)exc;
  ++g_counter;
  return xJSValueMakeUndefined(ctx);
}

static xJSValueRef native_counter_get(xJSContextRef ctx, xJSObjectRef fn,
                                      xJSObjectRef this_obj, size_t argc,
                                      const xJSValueRef argv[],
                                      xJSValueRef      *exc) {
  (void)fn;
  (void)this_obj;
  (void)argc;
  (void)argv;
  (void)exc;
  return xJSValueMakeNumber(ctx, (double)g_counter);
}

static xJSValueRef native_counter_reset(xJSContextRef ctx, xJSObjectRef fn,
                                        xJSObjectRef this_obj, size_t argc,
                                        const xJSValueRef argv[],
                                        xJSValueRef      *exc) {
  (void)fn;
  (void)this_obj;
  (void)argc;
  (void)argv;
  (void)exc;
  g_counter = 0;
  return xJSValueMakeUndefined(ctx);
}

/* A minimal console.log: stringify every argument, join with ' ',
 * print with a trailing newline.  Good enough to prove the module
 * wiring end-to-end. */
static xJSValueRef native_console_log(xJSContextRef ctx, xJSObjectRef fn,
                                      xJSObjectRef this_obj, size_t argc,
                                      const xJSValueRef argv[],
                                      xJSValueRef      *exc) {
  (void)fn;
  (void)this_obj;
  (void)exc;
  for (size_t i = 0; i < argc; i++) {
    xJSStringRef s = xJSValueToStringCopy(ctx, argv[i], NULL);
    if (!s) continue;
    size_t sz  = xJSStringGetMaximumUTF8CStringSize(s);
    char  *buf = (char *)malloc(sz);
    if (buf) {
      xJSStringGetUTF8CString(s, buf, sz);
      fputs(buf, stdout);
      if (i + 1 < argc) fputc(' ', stdout);
      free(buf);
    }
    xJSStringRelease(s);
  }
  fputc('\n', stdout);
  return xJSValueMakeUndefined(ctx);
}

/* ───────────────────── wiring helpers ───────────────────────────
 *
 * Install a native function as `globalThis[holder_key][fn_key]`,
 * creating the holder object on first use.  The holder sits under
 * a double-underscored name to signal "implementation detail — use
 * the import facade instead".
 */

static void install_native(xJSGlobalContextRef ctx, const char *holder_key,
                           const char                     *fn_key,
                           xJSObjectCallAsFunctionCallback cb) {
  xJSObjectRef global = xJSContextGetGlobalObject(ctx);

  /* Get-or-create holder. */
  xJSStringRef hk = xJSStringCreateWithUTF8CString(holder_key);
  xJSValueRef  hv = xJSObjectGetProperty(ctx, global, hk, NULL);
  xJSObjectRef holder;
  if (!hv || xJSValueIsUndefined(ctx, hv) || !xJSValueIsObject(ctx, hv)) {
    holder = xJSObjectMake(ctx, NULL, NULL);
    xJSObjectSetProperty(ctx, global, hk, (xJSValueRef)holder, 0, NULL);
  } else {
    holder = (xJSObjectRef)hv;
  }

  xJSStringRef fnk = xJSStringCreateWithUTF8CString(fn_key);
  xJSObjectRef fn  = xJSObjectMakeFunctionWithCallback(ctx, fnk, cb);
  xJSObjectSetProperty(ctx, holder, fnk, (xJSValueRef)fn, 0, NULL);

  xJSStringRelease(fnk);
  xJSStringRelease(hk);
}

/* ───────────────────── module loader ────────────────────────────
 *
 * For each bare specifier we recognise, return a JS source string
 * that re-exports entries of the corresponding global hook under
 * ergonomic names.  Unknown specifiers return NULL → the engine
 * raises a ReferenceError at link time.
 *
 * Note: because the facade source is *pure JS*, QuickJS performs
 * all the heavy lifting (binding resolution, cycle handling,
 * top-level await).  No manual JSModuleDef plumbing required.
 */

static xJSStringRef load_native_module(xJSContextRef ctx, const char *name,
                                       void *opaque) {
  (void)ctx;
  (void)opaque;

  if (strcmp(name, "counter") == 0) {
    static const char src[] = "const H = globalThis.__native_counter;\n"
                              "export const increment = H.inc;\n"
                              "export const get       = H.get;\n"
                              "export const reset     = H.reset;\n";
    return xJSStringCreateWithUTF8CString(src);
  }

  if (strcmp(name, "console") == 0) {
    static const char src[] = "const H = globalThis.__native_console;\n"
                              "export const log = H.log;\n";
    return xJSStringCreateWithUTF8CString(src);
  }

  return NULL; /* unknown specifier */
}

/* ───────────────────── main ─────────────────────────────────────
 *
 * Drives the full pipeline: create a context, install the two
 * native hooks, wire up the loader, evaluate a user module that
 * imports both, and block until the resulting promise settles.
 */

static void print_exception(xJSContextRef ctx, xJSValueRef exc,
                            const char *where) {
  if (!exc) {
    fprintf(stderr, "%s: (no exception object)\n", where);
    return;
  }
  xJSStringRef s = xJSValueToStringCopy(ctx, exc, NULL);
  if (!s) {
    fprintf(stderr, "%s: <unprintable exception>\n", where);
    return;
  }
  size_t sz  = xJSStringGetMaximumUTF8CStringSize(s);
  char  *buf = (char *)malloc(sz);
  if (buf) {
    xJSStringGetUTF8CString(s, buf, sz);
    fprintf(stderr, "%s: %s\n", where, buf);
    free(buf);
  }
  xJSStringRelease(s);
}

int main(void) {
  xJSGlobalContextRef ctx = xJSGlobalContextCreate(NULL);
  if (!ctx) {
    fputs("failed to create context\n", stderr);
    return 1;
  }

  /* 1. Install native hooks on the global object. */
  install_native(ctx, "__native_counter", "inc", native_counter_inc);
  install_native(ctx, "__native_counter", "get", native_counter_get);
  install_native(ctx, "__native_counter", "reset", native_counter_reset);
  install_native(ctx, "__native_console", "log", native_console_log);

  /* 2. Install the facade-synthesising loader.  No opaque needed —
   *    the loader is stateless. */
  xJSContextSetModuleLoader(ctx, load_native_module, NULL);

  /* 3. Evaluate a module that imports the facades.  Passing a
   *    sourceURL of "demo.js" gives clean stack traces. */
  static const char user_src[] =
    "import { increment, get, reset } from 'counter';\n"
    "import { log }                    from 'console';\n"
    "\n"
    "log('initial =', get());\n"
    "for (let i = 0; i < 5; i++) increment();\n"
    "log('after 5 inc =', get());\n"
    "reset();\n"
    "log('after reset =', get());\n";

  xJSStringRef src     = xJSStringCreateWithUTF8CString(user_src);
  xJSStringRef url     = xJSStringCreateWithUTF8CString("demo.js");
  xJSValueRef  exc     = NULL;
  xJSValueRef  promise = xJSEvaluateModule(ctx, src, url, &exc);
  xJSStringRelease(src);
  xJSStringRelease(url);

  if (!promise) {
    print_exception(ctx, exc, "module compile/link");
    if (exc) xJSValueUnprotect(ctx, exc);
    xJSGlobalContextRelease(ctx);
    return 1;
  }

  /* 4. Drive the event loop until the module's top-level finishes. */
  xJSValueRef result = xJSAwaitPromise(ctx, promise, &exc);
  if (!result) {
    print_exception(ctx, exc, "module runtime");
    if (exc) xJSValueUnprotect(ctx, exc);
    xJSValueUnprotect(ctx, promise);
    xJSGlobalContextRelease(ctx);
    return 1;
  }

  xJSValueUnprotect(ctx, result);
  xJSValueUnprotect(ctx, promise);
  xJSGlobalContextRelease(ctx);
  return 0;
}
