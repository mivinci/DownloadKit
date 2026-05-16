/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.h - Asynchronous HTTP/1.1 server powered by llhttp + xEventLoop
 *
 * Integrates llhttp with xEventLoop to provide a single-threaded,
 * non-blocking HTTP server. All callbacks are dispatched on the
 * event loop thread.
 */

#ifndef XHTTP_SERVER_H
#define XHTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/net/tls.h>

/* ── Types ─────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque handle to an HTTP server bound to an xEventLoop.
 */
XDEF_HANDLE(xHttpServer);

/**
 * @brief Opaque handle to a response writer used inside a handler.
 */
XDEF_HANDLE(xHttpResponseWriter);

/**
 * @brief HTTP request delivered to a handler callback.
 *
 * All pointers are valid only for the duration of the handler callback.
 * The caller must NOT free any of the fields; the library manages
 * their lifetime.
 */
XDEF_STRUCT(xHttpRequest) {
  const char *method;      /**< HTTP method string (e.g. "GET", "POST")    */
  const char *url;         /**< Request URL / path (NUL-terminated)        */
  const char *headers;     /**< Raw request headers (NUL-terminated)       */
  size_t      headers_len; /**< Length of headers in bytes                 */
  const char *body;        /**< Request body, or NULL if no body           */
  size_t      body_len;    /**< Length of body in bytes                    */
  void       *params_;     /**< (internal) matched route params            */
};

/**
 * @brief Handler callback invoked when a request matches a route.
 *
 * @param req     The parsed HTTP request (valid only during the callback).
 * @param writer  Response writer for building and sending the response.
 * @param arg     User-provided argument from xHttpServerRoute().
 */
typedef void (*xHttpHandlerFunc)(xHttpResponseWriter writer,
                                 const xHttpRequest *req, void *arg);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP server bound to an event loop.
 *
 * Allocates server state and binds it to the given event loop.
 * No listening socket is created until xHttpServerListen() is called.
 *
 * @param loop  The event loop (must not be NULL).
 * @return      A new server handle, or NULL on failure.
 */
XCAPI(xHttpServer) xHttpServerCreate(xEventLoop loop);

/**
 * @brief Start listening for HTTP connections.
 *
 * Creates a TCP listening socket with SO_REUSEADDR on the specified
 * address and port, and begins accepting connections.
 *
 * @param server  The HTTP server (must not be NULL).
 * @param host    Bind address (e.g. "0.0.0.0"), or NULL for INADDR_ANY.
 * @param port    Port number to listen on.
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpServerListen(xHttpServer server, const char *host,
                                uint16_t port);

/**
 * @brief Destroy an HTTP server and release all resources.
 *
 * Closes the listening socket, gracefully shuts down all active
 * connections, frees all routes, and releases the server handle.
 * Safe to call with NULL (no-op).
 *
 * @param server  The server to destroy, or NULL.
 */
XCAPI(void) xHttpServerDestroy(xHttpServer server);

/* ── Routing ───────────────────────────────────────────────────────────── */

/**
 * @brief Register a route (pattern → handler).
 *
 * The @p pattern string combines an optional HTTP method and a path,
 * following the Go `http.HandleFunc` convention:
 *
 *   - `"GET /users/:id"` — matches only GET requests to `/users/:id`.
 *   - `"/users/:id"`     — matches **all** HTTP methods to `/users/:id`.
 *
 * If the pattern starts with `'/'`, it matches any method.  Otherwise the
 * first space-delimited token is taken as the method and the remainder as
 * the path.
 *
 * Routes are matched in registration order (first match wins).
 * Must be called before xHttpServerListen().
 *
 * @param server   The HTTP server (must not be NULL).
 * @param pattern  Method + path pattern (must not be NULL, see above).
 * @param handler  Handler callback (must not be NULL).
 * @param arg      User argument forwarded to @p handler.
 * @return         xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpServerRoute(xHttpServer server, const char *pattern,
                               xHttpHandlerFunc handler, void *arg);

/**
 * @brief Look up a path parameter by name.
 *
 * For a route registered as "/users/:id", calling
 * xHttpRequestParam(req, "id") returns the matched segment value.
 *
 * @param req   The HTTP request (must not be NULL).
 * @param name  Parameter name without the leading ':' (must not be NULL).
 * @param len   If non-NULL, receives the length of the returned value.
 * @return      Pointer to the parameter value (NOT NUL-terminated), or
 *              NULL if the parameter was not found.
 */
XCAPI(const char *) xHttpRequestParam(const xHttpRequest *req, const char *name,
                                      size_t *len);

/* ── Response ──────────────────────────────────────────────────────────── */

/**
 * @brief Set the HTTP response status code.
 *
 * If not called, the default status is 200 OK.
 *
 * @param writer  The response writer (must not be NULL).
 * @param code    HTTP status code (e.g. 200, 404, 500).
 */
XCAPI(void) xHttpResponseSetStatus(xHttpResponseWriter writer, int code);

/**
 * @brief Add a response header.
 *
 * May be called multiple times to add multiple headers.
 * Must be called before xHttpResponseSend().
 *
 * @param writer  The response writer (must not be NULL).
 * @param key     Header name (must not be NULL).
 * @param value   Header value (must not be NULL).
 * @return        xErrno_Ok on success, xErrno_NoMemory on failure.
 */
XCAPI(xErrno) xHttpResponseSetHeader(xHttpResponseWriter writer,
                                     const char *key, const char *value);

/**
 * @brief Send the HTTP response.
 *
 * Serializes the status line, headers, and body into the write buffer
 * and initiates sending. If body is NULL and body_len is 0, an empty
 * body is sent.
 *
 * This function may only be called once per request. Subsequent calls
 * are no-ops. Mutually exclusive with xHttpResponseWrite().
 *
 * @param writer    The response writer (must not be NULL).
 * @param body      Response body data, or NULL.
 * @param body_len  Length of body in bytes.
 * @return          xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpResponseSend(xHttpResponseWriter writer, const char *body,
                                size_t body_len);

/**
 * @brief Write data to a streaming response.
 *
 * On the first call, the status line and headers are flushed (without
 * Content-Length). Subsequent calls append data directly. This is
 * suitable for Server-Sent Events (SSE) or chunked streaming.
 *
 * Mutually exclusive with xHttpResponseSend().
 *
 * @param writer  The response writer (must not be NULL).
 * @param data    Data to write.
 * @param len     Length of data in bytes.
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpResponseWrite(xHttpResponseWriter writer, const char *data,
                                 size_t len);

/**
 * @brief End a streaming response.
 *
 * Signals that no more data will be written. If the handler returns
 * without calling this, the stream is ended automatically.
 *
 * Only meaningful after xHttpResponseWrite() has been called.
 *
 * @param writer  The response writer (must not be NULL).
 */
XCAPI(void) xHttpResponseEnd(xHttpResponseWriter writer);

/* ── Configuration ─────────────────────────────────────────────────────── */

/**
 * @brief Set the idle timeout for connections.
 *
 * Connections that receive no data within this period are automatically
 * closed. Must be called before xHttpServerListen().
 *
 * @param server      The HTTP server (must not be NULL).
 * @param timeout_ms  Idle timeout in milliseconds (default: 60000).
 *                    Must be > 0.
 * @return            xErrno_Ok on success, xErrno_InvalidArg if invalid.
 */
XCAPI(xErrno) xHttpServerSetIdleTimeout(xHttpServer server, int timeout_ms);

/**
 * @brief Set the maximum allowed size for request headers.
 *
 * Requests with headers exceeding this limit receive a 431 response.
 * Must be called before xHttpServerListen().
 *
 * @param server    The HTTP server (must not be NULL).
 * @param max_size  Maximum header size in bytes (default: 8192).
 *                  Must be > 0.
 * @return          xErrno_Ok on success, xErrno_InvalidArg if invalid.
 */
XCAPI(xErrno) xHttpServerSetMaxHeaderSize(xHttpServer server, size_t max_size);

/**
 * @brief Set the maximum allowed size for request bodies.
 *
 * Requests with bodies exceeding this limit receive a 413 response.
 * Must be called before xHttpServerListen().
 *
 * @param server    The HTTP server (must not be NULL).
 * @param max_size  Maximum body size in bytes (default: 1048576).
 *                  Must be > 0.
 * @return          xErrno_Ok on success, xErrno_InvalidArg if invalid.
 */
XCAPI(xErrno) xHttpServerSetMaxBodySize(xHttpServer server, size_t max_size);

/* ── TLS ───────────────────────────────────────────────────────────────── */

/**
 * @brief Backward-compatible alias for xTlsConf (defined in xnet/tls.h).
 */
typedef xTlsConf xHttpTlsServerConf;

/**
 * @brief Start listening for HTTPS connections with TLS.
 *
 * Creates a TLS context using the provided certificate and key, then
 * begins accepting TLS connections on the specified address and port.
 * ALPN negotiation is used to select HTTP/1.1 or HTTP/2.
 *
 * Can be called alongside xHttpServerListen() to serve both HTTP and
 * HTTPS on different ports.
 *
 * If no TLS library was available at compile time, this function returns
 * xErrno_NotSupported.
 *
 * @param server  The HTTP server (must not be NULL).
 * @param host    Bind address (e.g. "0.0.0.0"), or NULL for INADDR_ANY.
 * @param port    Port number to listen on.
 * @param config  TLS configuration (must not be NULL, cert and
 *                key must not be NULL).
 * @return        xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xHttpServerListenTls(xHttpServer server, const char *host,
                                   uint16_t                  port,
                                   const xTlsConf *config);

#endif /* XHTTP_SERVER_H */
