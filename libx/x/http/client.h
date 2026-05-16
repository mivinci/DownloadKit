/*
 * Copyright 2025 The libx Authors. All rights reserved.
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
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/net/tls.h>

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
  long        status_code; /**< HTTP status code (e.g. 200), 0 on failure */
  const char *headers;     /**< Raw response headers (NUL-terminated)     */
  size_t      headers_len; /**< Length of headers in bytes                 */
  const char *body;        /**< Response body (NUL-terminated)             */
  size_t      body_len;    /**< Length of body in bytes                    */
  int         curl_code;   /**< CURLcode (0 = CURLE_OK on success)        */
  const char *curl_error;  /**< Human-readable curl error, or NULL        */
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
  xHttpMethod_GET = 0,    xHttpMethod_POST = 1,  xHttpMethod_PUT = 2,
  xHttpMethod_DELETE = 3, xHttpMethod_PATCH = 4, xHttpMethod_HEAD = 5,
};

/**
 * @brief HTTP version preference for requests.
 *
 * Controls which HTTP protocol version libcurl will use.
 * Zero-initialized structs default to xHttpVersion_Default.
 */
XDEF_ENUM(xHttpVersion){
  xHttpVersion_Default = 0, /**< Use client default (initially HTTP/1.1)    */
  xHttpVersion_H1      = 1, /**< Force HTTP/1.1                              */
  xHttpVersion_H2      = 2, /**< HTTP/2 with TLS (ALPN), fallback to H1      */
  xHttpVersion_H2TLS   = 3, /**< HTTP/2 over TLS only, no fallback           */
  xHttpVersion_H2C     = 4, /**< HTTP/2 cleartext (Prior Knowledge)           */
};

/**
 * @brief Configuration for a custom HTTP request.
 *
 * Used with xHttpClientDo() for full control over the request.
 * Zero-initialize for defaults (GET, no headers, no timeout).
 */
XDEF_STRUCT(xHttpRequestConf) {
  const char  *url;          /**< Request URL (must not be NULL)             */
  xHttpMethod  method;       /**< HTTP method (default: GET)                 */
  const char  *body;         /**< Request body, or NULL                      */
  size_t       body_len;     /**< Length of body in bytes                    */
  const char **headers;      /**< NULL-terminated array of "Key: Value"      */
  long         timeout_ms;   /**< Per-request timeout in ms (0 = no limit).
                                  For regular HTTP: total transfer timeout.
                                  For SSE: connection-phase timeout only;
                                  stalled streams are detected via
                                  low-speed-time instead.                  */
  xHttpVersion http_version; /**< HTTP version (0 = use client default)      */
};

/**
 * @brief Backward-compatible alias for xTlsConf (defined in xnet/tls.h).
 */
typedef xTlsConf xHttpTlsClientConf;

/**
 * @brief Configuration for creating an HTTP client.
 *
 * Zero-initialize for defaults (no TLS, HTTP/1.1).
 * Pass NULL to xHttpClientCreate() for the same defaults.
 */
XDEF_STRUCT(xHttpClientConf) {
  const xTlsConf *tls;          /**< TLS config, or NULL         */
  xHttpVersion          http_version; /**< Default HTTP version (0=H1) */
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP client bound to an event loop.
 *
 * Initialises a curl multi handle and registers socket/timer callbacks
 * with the given event loop. If @p conf is not NULL, the client is
 * configured with the given TLS and HTTP version settings.
 *
 * @param loop  The event loop (must not be NULL).
 * @param conf  Client configuration, or NULL for defaults.
 * @return      A new client handle, or NULL on failure.
 */
XCAPI(xHttpClient) xHttpClientCreate(xEventLoop loop,
                                     const xHttpClientConf *conf);

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
XCAPI(xErrno) xHttpClientDo(xHttpClient client, const xHttpRequestConf *config,
                            xHttpResponseFunc on_response, void *arg);

/* ── SSE (Server-Sent Events) ──────────────────────────────────────────── */

/**
 * @brief SSE event delivered to the callback.
 *
 * All strings are NUL-terminated and valid only during the callback.
 * The caller must copy if needed.
 */
XDEF_STRUCT(xSseEvent) {
  const char *event; /**< event type, "message" if omitted     */
  const char *data;  /**< event data (may be multiline)        */
  const char *id;    /**< last event ID, or NULL               */
  int         retry; /**< retry delay in ms, or -1 if omitted  */
};

/**
 * @brief Callback invoked for each SSE event.
 *
 * @param ev   The SSE event (valid only during the callback).
 * @param arg  User-provided argument.
 * @return     0 to continue, non-zero to close the connection.
 */
typedef int (*xSseEventFunc)(const xSseEvent *ev, void *arg);

/**
 * @brief Callback invoked when the SSE stream ends.
 *
 * @param curl_code  CURLcode (0 = clean close, non-zero = error).
 * @param arg        User-provided argument.
 */
typedef void (*xSseDoneFunc)(int curl_code, void *arg);

/**
 * @brief Subscribe to an SSE endpoint.
 *
 * Sets Accept: text/event-stream and parses the stream according to
 * the W3C SSE specification.
 *
 * @param client    The HTTP client.
 * @param url       SSE endpoint URL.
 * @param on_event  Callback for each event (must not be NULL).
 * @param on_done   Callback when stream ends (may be NULL).
 * @param arg       User argument forwarded to callbacks.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xHttpClientGetSse(xHttpClient client, const char *url,
                                xSseEventFunc on_event, xSseDoneFunc on_done,
                                void *arg);

/**
 * @brief Submit a fully-configured SSE request.
 *
 * Like xHttpClientGetSse(), but uses xHttpRequestConf for full control
 * over the HTTP method, headers, and body. This is useful for LLM APIs
 * that require POST with a JSON body and custom Authorization headers.
 *
 * The Accept: text/event-stream header is added automatically.
 *
 * @param client    The HTTP client.
 * @param config    Request configuration (must not be NULL, config->url
 *                  must not be NULL).
 * @param on_event  Callback for each event (must not be NULL).
 * @param on_done   Callback when stream ends (may be NULL).
 * @param arg       User argument forwarded to callbacks.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xHttpClientDoSse(xHttpClient             client,
                               const xHttpRequestConf *config,
                               xSseEventFunc on_event, xSseDoneFunc on_done,
                               void *arg);

#endif /* XHTTP_CLIENT_H */
