/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * curl.h - libcurl integration for xEventLoop
 *
 * xCurlMulti binds a libcurl multi handle to an xEventLoop so that HTTP
 * requests are driven entirely by the event loop without extra threads.
 * All done callbacks are invoked on the event-loop thread.
 *
 * Typical usage:
 *
 *   xEventLoop loop = xEventLoopCreate();
 *   xCurlMulti m    = xCurlMultiNew(loop);
 *
 *   xCurlMultiGet(m, "https://example.com", my_done_cb, ctx);
 *
 *   xEventLoopRun(loop);   // drives both I/O and HTTP
 */

#ifndef XBASE_CURL_H
#define XBASE_CURL_H

#include <xbase/base.h>
#include <xbase/event.h>

/* Opaque handle for a libcurl multi context bound to an xEventLoop. */
XDEF_HANDLE(xCurlMulti);

/*
 * xCurlDoneFunc - completion callback.
 *
 * @arg       user-supplied pointer passed to xCurlMultiGet/Post
 * @http_code HTTP status code (e.g. 200, 404); 0 on transport error
 * @err_code  libcurl CURLcode; CURLE_OK (0) on success
 * @err_msg   human-readable error string; empty string on success
 *
 * Always called on the event-loop thread.
 */
typedef void (*xCurlDoneFunc)(void *arg, long http_code,
                               int err_code, const char *err_msg);

/* Create a new xCurlMulti bound to the given event loop. Returns NULL on error. */
XCAPI(xCurlMulti) xCurlMultiNew(xEventLoop loop);

/* Destroy an xCurlMulti and cancel any pending timer. In-flight requests are
 * not cancelled; their done callbacks will not be invoked after this call. */
XCAPI(void) xCurlMultiDestroy(xCurlMulti m);

/* Enqueue an async HTTP GET. done_fn is called on completion. */
XCAPI(xErrno) xCurlMultiGet(xCurlMulti m, const char *url,
                              xCurlDoneFunc done_fn, void *arg);

/* Enqueue an async HTTP POST with the given body (may be NULL for empty body). */
XCAPI(xErrno) xCurlMultiPost(xCurlMulti m, const char *url,
                               const void *body, size_t body_len,
                               xCurlDoneFunc done_fn, void *arg);

#endif
