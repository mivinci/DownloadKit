/*
 * Copyright 2025 The x Kit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xcurl.c - libcurl integration for xEventLoop
 */

#include "xcurl.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ───────────────────── Concrete struct definitions ───────────────────── */

struct xCurlMulti_ {
    xEventLoop loop;         /* callers event loop */
    xTaskGroup group;        /* callers task group */
    CURLM     *multi;
    void      *userp;
    xEventSource *wake_src;
    int        wake_wfd;
    int        wake_rfd;
};

struct xCurlRequest_ {
    xCurlMulti multi;
    CURL      *easy;
    char       url[512];
    xCurlDoneFunc done_fn;
    void      *done_arg;
    xEventSource *src;
    xEventTimer  timer;
};

struct xCurlDoneCtx_ {
    xCurlDoneFunc fn;
    void         *arg;
    long          http_code;
    int           curl_code;
    char          errbuf[CURL_ERROR_SIZE];
};

/* ───────────────────── Forward declarations ───────────────────── */

static int curl_socket_cb(CURL *easy, curl_socket_t fd, int what, void *userp);
static int curl_timer_cb(CURLM *multi, long timeout_ms, void *userp);
static void on_socket(int fd, xEventMask mask, void *arg);
static void on_done_wrapper(void *arg, void *result);

/* ───────────────────── Helpers ───────────────────── */

static struct xCurlRequest_ *req_from_easy(CURL *easy) {
    char *p = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &p);
    return (struct xCurlRequest_ *)p;
}

/* ───────────────────── Drive multi ───────────────────── */

static void drive_multi(struct xCurlMulti_ *m, int socket_action) {
    int running = 0;
    curl_multi_socket_action(m->multi, CURL_SOCKET_TIMEOUT, 0, &running);

    CURLMsg *msg = NULL;
    int nmsg = 0;
    while ((msg = curl_multi_info_read(m->multi, &nmsg)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL *easy = msg->easy_handle;
        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

        struct xCurlRequest_ *req = req_from_easy(easy);
        if (req && req->done_fn) {
            struct xCurlDoneCtx_ *ctx = calloc(1, sizeof(*ctx));
            if (ctx) {
                ctx->fn       = req->done_fn;
                ctx->arg       = req->done_arg;
                ctx->http_code = http_code;
                ctx->curl_code = msg->data.result;
                snprintf(ctx->errbuf, sizeof(ctx->errbuf), "%s",
                         curl_easy_strerror(msg->data.result));

                xEventLoopSubmit(m->loop, m->group,
                                 (xTaskFunc)ctx->fn, on_done_wrapper, ctx);
            }
        }

        curl_multi_remove_handle(m->multi, easy);
        curl_easy_cleanup(easy);
        if (req) {
            if (req->src)   { xEventDel(m->loop, req->src);   req->src   = NULL; }
            if (req->timer) { xEventLoopTimerCancel(m->loop, req->timer); req->timer = NULL; }
            free(req);
        }
    }
}

/* ───────────────────── curl callbacks ───────────────────── */

static int curl_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp) {
    (void)s;
    struct xCurlMulti_ *m = userp;
    if (!m) return 0;

    struct xCurlRequest_ *req = req_from_easy(easy);
    if (!req) return 0;

    switch (what) {
        case CURL_POLL_IN:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Read,  on_socket, req);
            break;
        case CURL_POLL_OUT:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Write, on_socket, req);
            break;
        case CURL_POLL_INOUT:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Read|xEvent_Write, on_socket, req);
            break;
        case CURL_POLL_REMOVE:
        default:
            if (req->src) { xEventDel(m->loop, req->src); req->src = NULL; }
            break;
    }
    return 0;
}

static int curl_timer_cb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    struct xCurlMulti_ *m = userp;
    if (!m) return 0;

    if (timeout_ms < 0) return 0;          /* no timeout */
    if (timeout_ms == 0) {
        drive_multi(m, 0);                 /* process now */
        return 0;
    }

    /* Schedule a timer to call drive_multi */
    xEventLoopTimerAfter(m->loop, (xEventTimerFunc)drive_multi,
                         m, (uint64_t)timeout_ms);
    return 0;
}

/* ───────────────────── socket event callback ───────────────────── */

static void on_socket(int fd, xEventMask mask, void *arg) {
    struct xCurlRequest_ *req = arg;
    if (!req || !req->multi) return;

    struct xCurlMulti_ *m = (struct xCurlMulti_ *)req->multi;
    int action = 0;
    if (mask & xEvent_Read)  action |= CURL_CSELECT_IN;
    if (mask & xEvent_Write) action |= CURL_CSELECT_OUT;
    (void)fd;

    drive_multi(m, action);
}

/* ───────────────────── Public API ───────────────────── */

xCurlMulti xCurlMultiNew(xEventLoop loop, xTaskGroup group) {
    if (!loop) return NULL;

    CURLM *multi = curl_multi_init();
    if (!multi) return NULL;

    struct xCurlMulti_ *m = calloc(1, sizeof(*m));
    if (!m) { curl_multi_cleanup(multi); return NULL; }

    m->loop  = loop;
    m->group = group ? group : xTaskGroupGlobal();
    m->multi = multi;
    m->userp = m;

    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
    curl_multi_setopt(multi, CURLMOPT_SOCKETDATA,     m);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION,  curl_timer_cb);
    curl_multi_setopt(multi, CURLMOPT_TIMERDATA,      m);

    int fds[2];
    if (pipe(fds) != 0) {
        curl_multi_cleanup(multi); free(m); return NULL;
    }
    set_nonblock(fds[0]);
    m->wake_rfd = fds[0];
    m->wake_wfd = fds[1];
    m->wake_src = xEventAdd(loop, fds[0], xEvent_Read, on_socket, m);
    if (!m->wake_src) {
        close(fds[0]); close(fds[1]);
        curl_multi_cleanup(multi); free(m); return NULL;
    }

    return (xCurlMulti)m;
}

void xCurlMultiDestroy(xCurlMulti m_) {
    if (!m_) return;
    struct xCurlMulti_ *m = (struct xCurlMulti_ *)m_;

    if (m->wake_src) {
        xEventDel(m->loop, m->wake_src);
        close(m->wake_rfd);
        close(m->wake_wfd);
    }
    curl_multi_cleanup(m->multi);
    free(m);
}

xErrno xCurlMultiGet(xCurlMulti m_, const char *url,
                        xCurlDoneFunc done_fn, void *arg) {
    return xCurlMultiPost(m_, url, NULL, 0, done_fn, arg);
}

xErrno xCurlMultiPost(xCurlMulti m_, const char *url,
                          const void *body, size_t body_len,
                          xCurlDoneFunc done_fn, void *arg) {
    if (!m_ || !url || !done_fn) return xErrno_InvalidArg;

    struct xCurlMulti_ *m = (struct xCurlMulti_ *)m_;
    CURL *easy = curl_easy_init();
    if (!easy) return xErrno_SysError;

    struct xCurlRequest_ *req = calloc(1, sizeof(*req));
    if (!req) { curl_easy_cleanup(easy); return xErrno_NoMemory; }

    req->multi    = m_;
    req->easy     = easy;
    req->done_fn  = done_fn;
    req->done_arg = arg;
    strncpy(req->url, url, sizeof(req->url) - 1);
    req->url[sizeof(req->url) - 1] = '\0';

    curl_easy_setopt(easy, CURLOPT_URL,               req->url);
    curl_easy_setopt(easy, CURLOPT_PRIVATE,           req);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS,       1L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL,         1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,        30000L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

    if (body && body_len > 0) {
        curl_easy_setopt(easy, CURLOPT_POST,           1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,    body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    CURLMcode mc = curl_multi_add_handle(m->multi, easy);
    if (mc != CURLM_OK) { free(req); curl_easy_cleanup(easy); return xErrno_SysError; }

    curl_multi_wakeup(m->multi);
    return xErrno_Ok;
}

static void on_done_wrapper(void *arg, void *result) {
    (void)result;
    struct xCurlDoneCtx_ *ctx = arg;
    if (!ctx) return;

    xCurlDoneFunc fn = ctx->fn;
    void *user_arg = ctx->arg;
    long http_code = ctx->http_code;
    int curl_code = ctx->curl_code;
    char errbuf[CURL_ERROR_SIZE];
    memcpy(errbuf, ctx->errbuf, sizeof(errbuf));
    free(ctx);

    if (fn) fn(user_arg, http_code, curl_code, errbuf);
}
