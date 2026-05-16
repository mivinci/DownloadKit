/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * model_test.cpp - Unit tests for xAgentModelRegistry.
 *
 * These tests do not need a real provider; the registry only stores
 * borrowed xAgentProvider pointers and never dereferences them. We
 * therefore use a stable dummy pointer (cast from a FakeImpl pointer)
 * to stand in for a provider handle.
 */

#include <gtest/gtest.h>

extern "C" {
#include <x/agent/model.h>
#include "provider_private.h"
}

#include <cstdlib>
#include <cstring>

/* A bare xAgentProvider_ whose vtable is never invoked — just needs
 * to be a distinct non-NULL pointer that outlives the registry. */
static xAgentProvider make_dummy_provider() {
  auto *base =
    static_cast<xAgentProvider_ *>(calloc(1, sizeof(xAgentProvider_)));
  base->vt  = nullptr;
  base->ctx = nullptr;
  return reinterpret_cast<xAgentProvider>(base);
}

static void free_dummy_provider(xAgentProvider p) { free(p); }

/* ── Basic lifecycle ─────────────────────────────────────────────────── */

TEST(XAgentModelRegistry, CreateDestroyEmpty) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  ASSERT_NE(reg, nullptr);
  EXPECT_EQ(xAgentModelRegistryCount(reg), 0u);
  EXPECT_EQ(xAgentModelRegistryGet(reg, "anything"), nullptr);
  EXPECT_EQ(xAgentModelRegistryAt(reg, 0), nullptr);
  xAgentModelRegistryDestroy(reg);
}

TEST(XAgentModelRegistry, DestroyNullIsNoop) {
  xAgentModelRegistryDestroy(nullptr);
  /* Just exercising the NULL branch — no crash = pass. */
  SUCCEED();
}

/* ── Add / Get ───────────────────────────────────────────────────────── */

TEST(XAgentModelRegistry, AddAndGet) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  ASSERT_NE(reg, nullptr);

  xAgentProvider pvd = make_dummy_provider();

  xAgentModelSpec spec = {};
  spec.id          = "kimi";
  spec.provider    = pvd;
  spec.model       = "kimi-k2.6";
  spec.temperature = 0.7;
  spec.max_tokens  = 4096;

  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);
  EXPECT_EQ(xAgentModelRegistryCount(reg), 1u);

  const xAgentModelSpec *got = xAgentModelRegistryGet(reg, "kimi");
  ASSERT_NE(got, nullptr);
  EXPECT_STREQ(got->id, "kimi");
  EXPECT_STREQ(got->model, "kimi-k2.6");
  EXPECT_EQ(got->provider, pvd);
  EXPECT_DOUBLE_EQ(got->temperature, 0.7);
  EXPECT_EQ(got->max_tokens, 4096);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

TEST(XAgentModelRegistry, AddDeepCopiesStrings) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  /* Build id/model in mutable storage so we can scribble on it. */
  char id_buf[16];
  char model_buf[32];
  std::strcpy(id_buf, "glm");
  std::strcpy(model_buf, "glm-4.5");

  xAgentModelSpec spec = {};
  spec.id       = id_buf;
  spec.provider = pvd;
  spec.model    = model_buf;
  ASSERT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);

  /* Mutate the caller buffers — the registry must not be affected. */
  std::strcpy(id_buf, "CLOBBERED");
  std::strcpy(model_buf, "CLOBBERED-model");

  const xAgentModelSpec *got = xAgentModelRegistryGet(reg, "glm");
  ASSERT_NE(got, nullptr);
  EXPECT_STREQ(got->id, "glm");
  EXPECT_STREQ(got->model, "glm-4.5");

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

TEST(XAgentModelRegistry, AddNullModelIsAllowed) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  xAgentModelSpec spec = {};
  spec.id       = "default";
  spec.provider = pvd;
  spec.model    = nullptr; /* caller wants provider's own default */

  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);
  const xAgentModelSpec *got = xAgentModelRegistryGet(reg, "default");
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->model, nullptr);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

TEST(XAgentModelRegistry, GetReturnsNullForMissingId) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  xAgentModelSpec spec = {};
  spec.id       = "only";
  spec.provider = pvd;
  ASSERT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);

  EXPECT_EQ(xAgentModelRegistryGet(reg, "missing"), nullptr);
  EXPECT_EQ(xAgentModelRegistryGet(reg, nullptr), nullptr);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

/* ── Validation ──────────────────────────────────────────────────────── */

TEST(XAgentModelRegistry, AddRejectsBadArgs) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  EXPECT_EQ(xAgentModelRegistryAdd(nullptr, nullptr), xErrno_InvalidArg);
  EXPECT_EQ(xAgentModelRegistryAdd(reg, nullptr), xErrno_InvalidArg);

  xAgentModelSpec spec = {};
  /* Missing id */
  spec.id = nullptr; spec.provider = pvd;
  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_InvalidArg);

  /* Empty id */
  spec.id = ""; spec.provider = pvd;
  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_InvalidArg);

  /* Missing provider */
  spec.id = "foo"; spec.provider = nullptr;
  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_InvalidArg);

  EXPECT_EQ(xAgentModelRegistryCount(reg), 0u);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

TEST(XAgentModelRegistry, AddRejectsDuplicateId) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  xAgentModelSpec spec = {};
  spec.id = "kimi"; spec.provider = pvd; spec.model = "kimi-k2.6";
  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);

  /* Same id, different model — still rejected. */
  spec.model = "kimi-k3";
  EXPECT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_AlreadyExists);
  EXPECT_EQ(xAgentModelRegistryCount(reg), 1u);

  /* Ensure the original entry was not clobbered. */
  const xAgentModelSpec *got = xAgentModelRegistryGet(reg, "kimi");
  ASSERT_NE(got, nullptr);
  EXPECT_STREQ(got->model, "kimi-k2.6");

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

/* ── Iteration ──────────────────────────────────────────────────────── */

TEST(XAgentModelRegistry, AtReturnsEntriesInInsertionOrder) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  const char *ids[] = {"a", "b", "c", "d", "e"};
  for (const char *id : ids) {
    xAgentModelSpec spec = {};
    spec.id       = id;
    spec.provider = pvd;
    ASSERT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok);
  }

  EXPECT_EQ(xAgentModelRegistryCount(reg), 5u);
  for (size_t i = 0; i < 5; i++) {
    const xAgentModelSpec *got = xAgentModelRegistryAt(reg, i);
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got->id, ids[i]);
  }
  EXPECT_EQ(xAgentModelRegistryAt(reg, 5), nullptr);
  EXPECT_EQ(xAgentModelRegistryAt(reg, 1000), nullptr);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

TEST(XAgentModelRegistry, GrowsBeyondInitialCapacity) {
  xAgentModelRegistry reg = xAgentModelRegistryCreate();
  xAgentProvider      pvd = make_dummy_provider();

  /* Push well past the initial capacity of 4 to exercise realloc. */
  for (int i = 0; i < 20; i++) {
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "m%02d", i);
    xAgentModelSpec spec = {};
    spec.id       = idbuf;
    spec.provider = pvd;
    ASSERT_EQ(xAgentModelRegistryAdd(reg, &spec), xErrno_Ok) << "i=" << i;
  }
  EXPECT_EQ(xAgentModelRegistryCount(reg), 20u);

  /* Spot-check a few. */
  EXPECT_NE(xAgentModelRegistryGet(reg, "m00"), nullptr);
  EXPECT_NE(xAgentModelRegistryGet(reg, "m19"), nullptr);

  xAgentModelRegistryDestroy(reg);
  free_dummy_provider(pvd);
}

/* ── NULL-safety on accessors ───────────────────────────────────────── */

TEST(XAgentModelRegistry, AccessorsHandleNull) {
  EXPECT_EQ(xAgentModelRegistryGet(nullptr, "x"), nullptr);
  EXPECT_EQ(xAgentModelRegistryCount(nullptr), 0u);
  EXPECT_EQ(xAgentModelRegistryAt(nullptr, 0), nullptr);
}
