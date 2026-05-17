/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider_openai.h - OpenAI-compatible provider for the xai agent core
 *
 * Implements the xAgentProvider contract for any endpoint that speaks
 * the OpenAI /v1/chat/completions streaming protocol (OpenAI,
 * Together, DeepSeek, Moonshot, ollama's OpenAI mode, ...). The
 * transport is built on top of xHttpClientDoSse().
 */

#ifndef XAGENT_PROVIDER_OPENAI_H
#define XAGENT_PROVIDER_OPENAI_H

#include <x/agent/provider.h>
#include <x/base/base.h>
#include <x/base/event.h>
#include <x/http/client.h>

/**
 * @brief Configuration for an OpenAI-compatible provider.
 *
 * Zero-initialise for defaults (official OpenAI base URL, no
 * organization header, no default model, no timeout).
 */
XDEF_STRUCT(xAgentOpenAIConf) {
  const char *api_key;       /**< API key sent as Bearer token
                                  (must not be NULL).                    */
  const char *base_url;      /**< Base URL without trailing slash,
                                  e.g. "https://api.openai.com/v1".
                                  NULL = official OpenAI endpoint.       */
  const char *organization;  /**< Optional OpenAI-Organization header    */
  const char *default_model; /**< Used when submit conf has NULL model   */
  long        timeout_ms;    /**< Per-request timeout (0 = no limit)     */
};

/**
 * @brief Create an OpenAI-compatible provider bound to an event loop
 *        and an HTTP client.
 *
 * The provider does not take ownership of @p loop or @p http; both
 * must outlive every session that uses this provider.
 *
 * @param loop  Event loop (must not be NULL).
 * @param http  HTTP client used to issue streaming POSTs
 *              (must not be NULL, must be bound to @p loop).
 * @param conf  Provider configuration (must not be NULL).
 * @return      A new provider handle, or NULL on failure.
 */
XCAPI(xAgentProvider) xAgentProviderOpenAICreate(xEventLoop loop, xHttpClient http,
                                                 const xAgentOpenAIConf *conf);

#endif /* XAGENT_PROVIDER_OPENAI_H */
