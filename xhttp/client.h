/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - Asynchronous HTTP client powered by libcurl + xEventLoop
 *
 * Integrates libcurl's multi-socket API with xEventLoop to provide
 * a single-threaded, non-blocking HTTP client. All callbacks are
 * dispatched on the event loop thread.
 */

#ifndef XHTTP_CLIENT_H
#define XHTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>

/**
 * @brief Opaque handle to an HTTP client bound to an xEventLoop.
 */
XDEF_HANDLE(xHttpClient);

/**
 * @brief HTTP response delivered to the completion callback.
 *
 * All pointers are valid only for the duration of the callback.
 * The caller must NOT free any of the fields; the library manages
 * their lifetime.
 */
XDEF_STRUCT(xHttpResponse) {
  long        status_code;    /**< HTTP status code (e.g. 200), 0 on failure */
  const char *headers;        /**< Raw response headers (NUL-terminated)     */
  size_t      headers_len;    /**< Length of headers in bytes                 */
  const char *body;           /**< Response body (NUL-terminated)             */
  size_t      body_len;       /**< Length of body in bytes                    */
  int         curl_code;      /**< CURLcode (0 = CURLE_OK on success)        */
  const char *curl_error;     /**< Human-readable curl error, or NULL        */
};

/**
 * @brief Callback invoked when an HTTP request completes.
 * @param resp  Response data (valid only during the callback).
 * @param arg   User-provided argument.
 */
typedef void (*xHttpResponseFunc)(const xHttpResponse *resp, void *arg);

/**
 * @brief HTTP method constants.
 */
XDEF_ENUM(xHttpMethod){
  xHttpMethod_GET    = 0,
  xHttpMethod_POST   = 1,
  xHttpMethod_PUT    = 2,
  xHttpMethod_DELETE = 3,
  xHttpMethod_PATCH  = 4,
  xHttpMethod_HEAD   = 5,
};

/**
 * @brief Configuration for a custom HTTP request.
 *
 * Used with xHttpClientDo() for full control over the request.
 * Zero-initialize for defaults (GET, no headers, no timeout).
 */
XDEF_STRUCT(xHttpRequestConf) {
  const char  *url;           /**< Request URL (must not be NULL)             */
  xHttpMethod  method;        /**< HTTP method (default: GET)                 */
  const char  *body;          /**< Request body, or NULL                      */
  size_t       body_len;      /**< Length of body in bytes                    */
  const char **headers;       /**< NULL-terminated array of "Key: Value"      */
  long         timeout_ms;    /**< Per-request timeout in ms (0 = no limit)   */
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP client bound to an event loop.
 *
 * Initialises a curl multi handle and registers socket/timer callbacks
 * with the given event loop.
 *
 * @param loop  The event loop (must not be NULL).
 * @return      A new client handle, or NULL on failure.
 */
XCAPI(xHttpClient) xHttpClientCreate(xEventLoop loop);

/**
 * @brief Destroy an HTTP client and release all resources.
 *
 * Any in-flight requests are cancelled; their completion callbacks are
 * invoked with an error status before resources are freed.
 *
 * @param client  The client to destroy.
 */
XCAPI(void) xHttpClientDestroy(xHttpClient client);

/* ── Convenience request helpers ───────────────────────────────────────── */

/**
 * @brief Submit an asynchronous HTTP GET request.
 *
 * @param client       The HTTP client.
 * @param url          Request URL.
 * @param on_response  Completion callback.
 * @param arg          User argument forwarded to @p on_response.
 * @return             xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientGet(xHttpClient client, const char *url,
                              xHttpResponseFunc on_response, void *arg);

/**
 * @brief Submit an asynchronous HTTP POST request.
 *
 * @param client       The HTTP client.
 * @param url          Request URL.
 * @param body         Request body data.
 * @param body_len     Length of @p body in bytes.
 * @param on_response  Completion callback.
 * @param arg          User argument forwarded to @p on_response.
 * @return             xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientPost(xHttpClient client, const char *url,
                               const char *body, size_t body_len,
                               xHttpResponseFunc on_response, void *arg);

/* ── Generic request ───────────────────────────────────────────────────── */

/**
 * @brief Submit a fully-configured asynchronous HTTP request.
 *
 * @param client       The HTTP client.
 * @param config       Request configuration (must not be NULL).
 * @param on_response  Completion callback.
 * @param arg          User argument forwarded to @p on_response.
 * @return             xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpClientDo(xHttpClient client,
                             const xHttpRequestConf *config,
                             xHttpResponseFunc on_response, void *arg);

#endif /* XHTTP_CLIENT_H */
