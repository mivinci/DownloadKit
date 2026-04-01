/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * curl.c - libcurl integration for xEventLoop
 *
 * Design overview
 * ───────────────
 * libcurl's multi interface is driven entirely from the outside: it never
 * blocks or polls on its own.  Instead it tells us two things via callbacks:
 *
 *   SOCKETFUNCTION (curl_socket_cb)
 *     Called whenever libcurl wants us to start/stop watching a file
 *     descriptor.  We forward those registrations to xEventAdd/xEventDel.
 *     When the fd becomes ready, xEventLoop calls on_socket(), which in turn
 *     calls curl_multi_socket_action() with the fd and the I/O direction
 *     (CURL_CSELECT_IN / CURL_CSELECT_OUT).
 *
 *   TIMERFUNCTION (curl_timer_cb)
 *     Called whenever libcurl wants us to schedule (or cancel) a timeout.
 *     We map this to xEventLoopTimerAfter().  When the timer fires,
 *     drive_multi_timeout() calls curl_multi_socket_action() with the
 *     special sentinel CURL_SOCKET_TIMEOUT, which tells libcurl to check
 *     for expired internal timeouts (connect timeout, transfer timeout, …).
 *
 * After every curl_multi_socket_action() call we drain curl_multi_info_read()
 * to collect completed transfers and invoke the user's done callback.
 * Because drive_multi() is always called from an xEventLoop callback we are
 * already on the event-loop thread, so the done callback is invoked directly
 * without any extra dispatch.
 */

#include "curl.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ───────────────────── Concrete struct definitions ───────────────────── */

struct xCurlMulti_ {
    xEventLoop    loop;
    CURLM        *multi;
    xEventSource *wake_src;   /* event source for the wake pipe read-end */
    int           wake_rfd;
    int           wake_wfd;
    xEventTimer   timer;      /* pending xEventLoopTimerAfter handle, or NULL */
};

struct xCurlRequest_ {
    xCurlMulti    multi;      /* back-pointer to owning xCurlMulti_ */
    CURL         *easy;
    xCurlDoneFunc done_fn;
    void         *done_arg;
    xEventSource *src;        /* event source for the curl socket fd */
};

/* ───────────────────── Forward declarations ───────────────────── */

static int  curl_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp);
static int  curl_timer_cb(CURLM *multi, long timeout_ms, void *userp);
static void on_socket(int fd, xEventMask mask, void *arg);
static void drive_multi(struct xCurlMulti_ *m, curl_socket_t s, int action);
static void drive_multi_timeout(void *arg);

/* ───────────────────── Helpers ───────────────────── */

static struct xCurlRequest_ *req_from_easy(CURL *easy) {
    char *p = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &p);
    return (struct xCurlRequest_ *)p;
}

/* ───────────────────── Core driver ───────────────────── */

/*
 * drive_multi - advance libcurl state machine and collect completed transfers.
 *
 * s / action: passed directly to curl_multi_socket_action().
 *   - For fd-ready events: s = the fd, action = CURL_CSELECT_IN/OUT.
 *   - For timer expiry:    s = CURL_SOCKET_TIMEOUT, action = 0.
 */
static void drive_multi(struct xCurlMulti_ *m, curl_socket_t s, int action) {
    int running = 0;
    curl_multi_socket_action(m->multi, s, action, &running);

    /* Drain completed transfers. */
    CURLMsg *msg;
    int      nmsg = 0;
    while ((msg = curl_multi_info_read(m->multi, &nmsg)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL *easy = msg->easy_handle;
        long  http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

        struct xCurlRequest_ *req = req_from_easy(easy);

        /* Invoke done callback on the event-loop thread (we're already here). */
        if (req && req->done_fn)
            req->done_fn(req->done_arg, http_code, msg->data.result,
                         curl_easy_strerror(msg->data.result));

        curl_multi_remove_handle(m->multi, easy);
        curl_easy_cleanup(easy);

        if (req) {
            if (req->src) { xEventDel(m->loop, req->src); req->src = NULL; }
            free(req);
        }
    }
}

/* Timer callback wrapper: called by xEventLoop when the scheduled timer fires. */
static void drive_multi_timeout(void *arg) {
    struct xCurlMulti_ *m = (struct xCurlMulti_ *)arg;
    m->timer = NULL;   /* timer has fired; clear the handle */
    drive_multi(m, CURL_SOCKET_TIMEOUT, 0);
}

/* ───────────────────── libcurl callbacks ───────────────────── */

/*
 * curl_socket_cb - libcurl tells us which fds to watch.
 *
 * what = CURL_POLL_IN    → watch for readability
 *      = CURL_POLL_OUT   → watch for writability
 *      = CURL_POLL_INOUT → watch for both
 *      = CURL_POLL_REMOVE → stop watching
 */
static int curl_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp) {
    struct xCurlMulti_   *m   = (struct xCurlMulti_ *)userp;
    struct xCurlRequest_ *req = req_from_easy(easy);
    if (!m || !req) return 0;

    /* Remove any existing watch before re-registering. */
    if (req->src) { xEventDel(m->loop, req->src); req->src = NULL; }

    switch (what) {
        case CURL_POLL_IN:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Read, on_socket, req);
            break;
        case CURL_POLL_OUT:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Write, on_socket, req);
            break;
        case CURL_POLL_INOUT:
            req->src = xEventAdd(m->loop, (int)s, xEvent_Read | xEvent_Write,
                                 on_socket, req);
            break;
        case CURL_POLL_REMOVE:
        default:
            /* Already removed above. */
            break;
    }
    return 0;
}

/*
 * curl_timer_cb - libcurl tells us when to fire the next timeout.
 *
 * timeout_ms < 0  → cancel any pending timer (no more timeouts needed)
 * timeout_ms = 0  → fire immediately
 * timeout_ms > 0  → schedule for timeout_ms milliseconds from now
 */
static int curl_timer_cb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    struct xCurlMulti_ *m = (struct xCurlMulti_ *)userp;
    if (!m) return 0;

    /* Cancel any previously scheduled timer first. */
    if (m->timer) {
        xEventLoopTimerCancel(m->loop, m->timer);
        m->timer = NULL;
    }

    if (timeout_ms < 0) return 0;   /* libcurl says: no timeout needed */

    if (timeout_ms == 0) {
        /* Fire immediately — libcurl needs to process something right now. */
        drive_multi(m, CURL_SOCKET_TIMEOUT, 0);
        return 0;
    }

    /* Schedule drive_multi_timeout() to run after timeout_ms milliseconds. */
    m->timer = xEventLoopTimerAfter(m->loop, drive_multi_timeout, m,
                                    (uint64_t)timeout_ms);
    return 0;
}

/* ───────────────────── xEventLoop fd-ready callback ───────────────────── */

static void on_socket(int fd, xEventMask mask, void *arg) {
    struct xCurlRequest_ *req = (struct xCurlRequest_ *)arg;
    if (!req || !req->multi) return;

    struct xCurlMulti_ *m = (struct xCurlMulti_ *)req->multi;
    int action = 0;
    if (mask & xEvent_Read)  action |= CURL_CSELECT_IN;
    if (mask & xEvent_Write) action |= CURL_CSELECT_OUT;

    drive_multi(m, (curl_socket_t)fd, action);
}

/* ───────────────────── Public API ───────────────────── */

xCurlMulti xCurlMultiNew(xEventLoop loop) {
    if (!loop) return NULL;

    CURLM *multi = curl_multi_init();
    if (!multi) return NULL;

    struct xCurlMulti_ *m = calloc(1, sizeof(*m));
    if (!m) { curl_multi_cleanup(multi); return NULL; }

    m->loop  = loop;
    m->multi = multi;

    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
    curl_multi_setopt(multi, CURLMOPT_SOCKETDATA,     m);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION,  curl_timer_cb);
    curl_multi_setopt(multi, CURLMOPT_TIMERDATA,      m);

    return (xCurlMulti)m;
}

void xCurlMultiDestroy(xCurlMulti m_) {
    if (!m_) return;
    struct xCurlMulti_ *m = (struct xCurlMulti_ *)m_;

    if (m->timer) { xEventLoopTimerCancel(m->loop, m->timer); m->timer = NULL; }
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

    curl_easy_setopt(easy, CURLOPT_URL,               url);
    curl_easy_setopt(easy, CURLOPT_PRIVATE,           req);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS,        1L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL,          1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,        30000L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

    if (body && body_len > 0) {
        curl_easy_setopt(easy, CURLOPT_POST,          1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,    body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    CURLMcode mc = curl_multi_add_handle(m->multi, easy);
    if (mc != CURLM_OK) { free(req); curl_easy_cleanup(easy); return xErrno_SysError; }

    /*
     * Kick libcurl to start the connection.  This triggers curl_socket_cb
     * (which registers the socket fd with xEventLoop) and curl_timer_cb
     * (which schedules the first timeout).  Without this initial kick,
     * libcurl would sit idle until the next external event.
     */
    int running = 0;
    curl_multi_socket_action(m->multi, CURL_SOCKET_TIMEOUT, 0, &running);

    return xErrno_Ok;
}
