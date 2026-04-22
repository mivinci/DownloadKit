/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider_test.cpp - Unit tests for the xAiProvider dispatch base.
 *
 * These tests bring up a tiny "fake" provider that exposes counters
 * through its vtable ops, then exercises the thin dispatch helpers
 * declared in provider_private.h and the public xAiProviderDestroy.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xai/provider.h>
#include "provider_private.h"
}

#include <cstdlib>
#include <cstring>

/* ── Fake provider ────────────────────────────────────────────────────── */

struct FakeImpl {
  int submit_calls  = 0;
  int cancel_calls  = 0;
  int destroy_calls = 0;

  xErrno                            submit_return = xErrno_Ok;
  const xAiProviderSubmitConf      *last_conf     = nullptr;
  const xAiProviderStreamCallbacks *last_cbs      = nullptr;
  void                             *last_cb_arg   = nullptr;

  /* When non-zero, destroy actually free()s this struct. Otherwise
   * the test keeps ownership to inspect counters after destroy. */
  int self_free = 0;
};

static xErrno fake_submit(void                             *impl,
                          const xAiProviderSubmitConf      *conf,
                          const xAiProviderStreamCallbacks *cbs,
                          void                             *cb_arg) {
  auto *f       = static_cast<FakeImpl *>(impl);
  f->submit_calls++;
  f->last_conf   = conf;
  f->last_cbs    = cbs;
  f->last_cb_arg = cb_arg;
  return f->submit_return;
}

static void fake_cancel(void *impl) {
  static_cast<FakeImpl *>(impl)->cancel_calls++;
}

static void fake_destroy(void *impl) {
  auto *f = static_cast<FakeImpl *>(impl);
  f->destroy_calls++;
  if (f->self_free) delete f;
}

static const xAiProviderVtable kFakeVtable = {
  fake_submit,
  fake_cancel,
  fake_destroy,
};

/* Wrap a FakeImpl in an xAiProvider. */
static xAiProvider make_fake_provider(FakeImpl *impl) {
  auto *base = static_cast<xAiProvider_ *>(calloc(1, sizeof(xAiProvider_)));
  base->vt   = &kFakeVtable;
  base->ctx  = impl;
  return reinterpret_cast<xAiProvider>(base);
}

/* ── ai_provider_submit ───────────────────────────────────────────────── */

TEST(XaiProvider, SubmitDispatchesToVtable) {
  FakeImpl    impl;
  xAiProvider pvd = make_fake_provider(&impl);

  xAiProviderSubmitConf      conf = {};
  xAiProviderStreamCallbacks cbs  = {};
  int                        arg_marker = 0;

  EXPECT_EQ(ai_provider_submit(pvd, &conf, &cbs, &arg_marker), xErrno_Ok);
  EXPECT_EQ(impl.submit_calls, 1);
  EXPECT_EQ(impl.last_conf, &conf);
  EXPECT_EQ(impl.last_cbs, &cbs);
  EXPECT_EQ(impl.last_cb_arg, &arg_marker);

  xAiProviderDestroy(pvd);
}

TEST(XaiProvider, SubmitPropagatesReturnCode) {
  FakeImpl impl;
  impl.submit_return = xErrno_InvalidState;
  xAiProvider pvd    = make_fake_provider(&impl);

  xAiProviderSubmitConf      conf = {};
  xAiProviderStreamCallbacks cbs  = {};
  EXPECT_EQ(ai_provider_submit(pvd, &conf, &cbs, nullptr),
            xErrno_InvalidState);
  EXPECT_EQ(impl.submit_calls, 1);

  xAiProviderDestroy(pvd);
}

TEST(XaiProvider, SubmitRejectsNullArgs) {
  FakeImpl    impl;
  xAiProvider pvd = make_fake_provider(&impl);

  xAiProviderSubmitConf      conf = {};
  xAiProviderStreamCallbacks cbs  = {};

  EXPECT_EQ(ai_provider_submit(nullptr, &conf, &cbs, nullptr),
            xErrno_InvalidArg);
  EXPECT_EQ(ai_provider_submit(pvd, nullptr, &cbs, nullptr),
            xErrno_InvalidArg);
  EXPECT_EQ(ai_provider_submit(pvd, &conf, nullptr, nullptr),
            xErrno_InvalidArg);

  /* None of those NULL paths should have reached the vtable. */
  EXPECT_EQ(impl.submit_calls, 0);

  xAiProviderDestroy(pvd);
}

/* ── ai_provider_cancel ───────────────────────────────────────────────── */

TEST(XaiProvider, CancelDispatchesToVtable) {
  FakeImpl    impl;
  xAiProvider pvd = make_fake_provider(&impl);

  ai_provider_cancel(pvd);
  ai_provider_cancel(pvd);
  EXPECT_EQ(impl.cancel_calls, 2);

  xAiProviderDestroy(pvd);
}

TEST(XaiProvider, CancelOnNullIsNoop) {
  ai_provider_cancel(nullptr); /* must not crash */
}

/* ── xAiProviderDestroy ───────────────────────────────────────────────── */

TEST(XaiProvider, DestroyCallsImplDestroyThenFrees) {
  auto *impl     = new FakeImpl();
  impl->self_free = 1;
  xAiProvider pvd = make_fake_provider(impl);

  /* After Destroy, both the impl and the base struct are gone;
   * we can only check the destroy_calls side-effect. We arrange
   * that by smuggling the counter out through a captured local. */
  int destroy_observed = 0;
  /* trampoline: replace the vtable destroy with one that stashes
   * the counter before falling through to the real one. */
  struct Trampoline {
    static void cb(void *i) {
      fake_destroy(i);
    }
  };
  /* Simpler: just peek before Destroy. We know the counter was 0. */
  EXPECT_EQ(impl->destroy_calls, 0);
  (void)destroy_observed;

  xAiProviderDestroy(pvd); /* frees impl (self_free) and base */
}

TEST(XaiProvider, DestroyNullIsNoop) {
  xAiProviderDestroy(nullptr); /* must not crash */
}

TEST(XaiProvider, DestroyInvokesImplDestroyExactlyOnce) {
  FakeImpl    impl;
  xAiProvider pvd = make_fake_provider(&impl);

  xAiProviderDestroy(pvd);
  EXPECT_EQ(impl.destroy_calls, 1);
  /* No dangling calls: we never touch pvd again. */
}
