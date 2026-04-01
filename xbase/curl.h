/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * curl.h - libcurl integration for xEventLoop
 */

#ifndef XBASE_CURL_H
#define XBASE_CURL_H

#include <curl/curl.h>
#include <xbase/base.h>
#include <xbase/event.h>
#include <xbase/task.h>

XDEF_HANDLE(xCurlMulti);
XDEF_HANDLE(xCurlRequest);

struct xCurlMulti_;
struct xCurlRequest_;

typedef void (*xCurlDoneFunc)(void *arg, long http_code,
                               int err_code, const char *err_msg);

XCAPI(xCurlMulti) xCurlMultiNew(xEventLoop loop, xTaskGroup group);
XCAPI(void)       xCurlMultiDestroy(xCurlMulti m);
XCAPI(xErrno) xCurlMultiGet(xCurlMulti m, const char *url,
                                xCurlDoneFunc done_fn, void *arg);
XCAPI(xErrno) xCurlMultiPost(xCurlMulti m, const char *url,
                                 const void *body, size_t body_len,
                                 xCurlDoneFunc done_fn, void *arg);

#endif
