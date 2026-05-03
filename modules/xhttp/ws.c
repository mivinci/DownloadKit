/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws.c - WebSocket connection implementation
 */

#include "server_private.h"
#include "ws_private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <xbase/log.h>
#include <xnet/compat.h>

/* Maximum iovec entries for writev */
#define XWS_MAX_IOV 64

/* ═══════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_on_event(xSocket sock, xEventMask mask, void *arg);
static void ws_try_flush(struct xWsConn_ *conn);
static void ws_process_frame(struct xWsConn_ *conn, xWsFrame *frame);
static void ws_fire_close(struct xWsConn_ *conn, uint16_t code,
                          const char *reason, size_t len);
static void ws_idle_timeout(void *arg);
static void ws_reset_idle_timer(struct xWsConn_ *conn);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection lifecycle
 * ═══════════════════════════════════════════════════════════════════════════
 */

struct xWsConn_ *xWsConnCreate(struct xHttpServer_ *server, xEventLoop loop,
                               xSocket sock, xTransport transport,
                               const xWsCallbacks *callbacks, void *arg,
                               int timeout_ms) {
  struct xWsConn_ *conn = (struct xWsConn_ *)calloc(1, sizeof(struct xWsConn_));
  if (!conn) return NULL;

  conn->server    = server;
  conn->loop      = loop;
  conn->sock      = sock;
  conn->transport = transport;

  xIOBufferInit(&conn->read_buf);
  xIOBufferInit(&conn->write_buf);
  xIOBufferInit(&conn->frag_buf);

  xWsFrameParserInit(&conn->parser, server ? 1 : 0);

  conn->close_state = xWsCloseState_Open;
  conn->in_fragment = 0;
  conn->is_client   = (server == NULL) ? 1 : 0;
  conn->writing     = 0;

  if (callbacks) {
    conn->callbacks = *callbacks;
  }
  conn->user_arg = arg;

  conn->idle_timeout_ms = timeout_ms;
  conn->idle_timer      = NULL;

  /* Add to server's WS connection list (server mode only) */
  if (server) {
    conn->prev = NULL;
    conn->next = server->ws_conns;
    if (server->ws_conns) {
      server->ws_conns->prev = conn;
    }
    server->ws_conns = conn;
  } else {
    conn->prev = NULL;
    conn->next = NULL;
  }

  /* Re-register socket with our WS event handler */
  xSocketSetCallback(sock, ws_on_event, conn);
  xSocketSetMask(loop, sock, xEvent_Read);

  /* Clear any HTTP-layer idle timeout timers that were set before
   * the WebSocket upgrade. The WS layer manages its own idle timer
   * via xEventLoopTimerAfter, so the socket-level timers from the
   * HTTP accept path must be cancelled to avoid use-after-free. */
  xSocketSetTimeout(sock, 0, 0);

  /* Start idle timer */
  ws_reset_idle_timer(conn);

  return conn;
}

void xWsConnDestroy(struct xWsConn_ *conn) {
  if (!conn) return;

  /* Remove from server's WS connection list */
  if (conn->prev) {
    conn->prev->next = conn->next;
  } else if (conn->server) {
    conn->server->ws_conns = conn->next;
  }
  if (conn->next) {
    conn->next->prev = conn->prev;
  }

  /* Cancel idle timer */
  if (conn->idle_timer) {
    xEventLoopTimerCancel(conn->loop, conn->idle_timer);
    conn->idle_timer = NULL;
  }

  /* Destroy transport */
  if (conn->transport.destroy) {
    conn->transport.destroy(conn->transport.ctx);
  }

  /* Destroy socket */
  if (conn->sock) {
    xSocketDestroy(conn->loop, conn->sock);
    conn->sock = NULL;
  }

#ifdef XHTTP_WS_DEFLATE
  /* Destroy deflate context */
  xWsDeflateDestroy(conn->deflate_ctx);
#endif

  /* Free buffers */
  xIOBufferDeinit(&conn->read_buf);
  xIOBufferDeinit(&conn->write_buf);
  xIOBufferDeinit(&conn->frag_buf);

  /* Free close reason */
  free(conn->close_reason);

  /* Free any partially parsed frame payload */
  free(conn->parser.frame.payload);

  free(conn);
}

void xWsConnClose(struct xWsConn_ *conn, uint16_t code, const char *reason,
                  size_t len) {
  if (conn->close_state != xWsCloseState_Open) return;

  conn->close_state = xWsCloseState_CloseSent;
  conn->close_code  = code;

  xWsFrameEncodeClose(&conn->write_buf, code, reason, len, conn->is_client);
  ws_try_flush(conn);

  /* Set a timeout for the peer's Close response */
  if (conn->idle_timer) {
    xEventLoopTimerCancel(conn->loop, conn->idle_timer);
  }
  conn->idle_timer =
    xEventLoopTimerAfter(conn->loop, ws_idle_timeout, conn, 5000);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════
 */

xErrno xWsSend(xWsConn handle, xWsOpcode opcode, const void *payload,
               size_t len) {
  struct xWsConn_ *conn = (struct xWsConn_ *)handle;
  if (!conn) return xErrno_InvalidArg;
  if (conn->close_state != xWsCloseState_Open) return xErrno_InvalidState;

  uint8_t op = (opcode == xWsOpcode_Text) ? XWS_OPCODE_TEXT : XWS_OPCODE_BINARY;

#ifdef XHTTP_WS_DEFLATE
  if (conn->deflate_ctx && len > 0) {
    uint8_t *compressed     = NULL;
    size_t   compressed_len = 0;
    if (xWsDeflateCompress(conn->deflate_ctx, (const uint8_t *)payload, len,
                           &compressed, &compressed_len) == 0) {
      int rc = xWsFrameEncodeEx(&conn->write_buf, 1, 1, op, compressed,
                                compressed_len, conn->is_client);
      free(compressed);
      if (rc < 0) return xErrno_NoMemory;
      ws_try_flush(conn);
      return xErrno_Ok;
    }
    /* Compression failed: fall through to uncompressed */
  }
#endif

  if (xWsFrameEncode(&conn->write_buf, 1, op, payload, len, conn->is_client) <
      0)
    return xErrno_NoMemory;

  ws_try_flush(conn);
  return xErrno_Ok;
}

xErrno xWsClose(xWsConn handle, uint16_t code) {
  struct xWsConn_ *conn = (struct xWsConn_ *)handle;
  if (!conn) return xErrno_InvalidArg;

  xWsConnClose(conn, code, NULL, 0);
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  I/O helpers
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_try_flush(struct xWsConn_ *conn) {
  if (xIOBufferEmpty(&conn->write_buf)) return;

  xnet_iovec iov[XWS_MAX_IOV];
  int        cnt = xIOBufferReadIov(&conn->write_buf, iov, XWS_MAX_IOV);
  if (cnt == 0) return;

  xnet_ssize_t n = conn->transport.writev(conn->transport.ctx, iov, cnt);
  if (n < 0) {
    if (xnet_errno() == XNET_EAGAIN || xnet_errno() == XNET_EWOULDBLOCK) {
      if (!conn->writing) {
        conn->writing = 1;
        xSocketSetMask(conn->loop, conn->sock, xEvent_Read | xEvent_Write);
      }
      return;
    }
    /* Write error: force close */
    ws_fire_close(conn, XWS_CLOSE_ABNORMAL, NULL, 0);
    return;
  }
  if (n > 0) xIOBufferConsume(&conn->write_buf, (size_t)n);

  if (!xIOBufferEmpty(&conn->write_buf)) {
    if (!conn->writing) {
      conn->writing = 1;
      xSocketSetMask(conn->loop, conn->sock, xEvent_Read | xEvent_Write);
    }
  } else {
    if (conn->writing) {
      conn->writing = 0;
      xSocketSetMask(conn->loop, conn->sock, xEvent_Read);
    }

    /* If we're in CLOSE_RECEIVED state and write buffer is
     * drained, we can destroy the connection */
    if (conn->close_state == xWsCloseState_CloseReceived) {
      ws_fire_close(conn, conn->close_code, conn->close_reason,
                    conn->close_reason_len);
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Close notification
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_fire_close(struct xWsConn_ *conn, uint16_t code,
                          const char *reason, size_t len) {
  if (conn->close_state == xWsCloseState_Closed) return;
  conn->close_state = xWsCloseState_Closed;

  if (conn->callbacks.on_close) {
    conn->callbacks.on_close((xWsConn)conn, code, reason, len, conn->user_arg);
  }

  /* NOTE: Do NOT call xWsConnDestroy here.
   * The caller (ws_on_event) may still reference `conn` after
   * this function returns (e.g. when both Write and Read events
   * fire in the same epoll iteration, or during the frame-parse
   * loop). Destruction is deferred to the end of ws_on_event. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Idle timeout
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_idle_timeout(void *arg) {
  struct xWsConn_ *conn = (struct xWsConn_ *)arg;
  conn->idle_timer      = NULL;

  if (conn->close_state == xWsCloseState_Open) {
    /* Send Close frame with 1001 Going Away */
    xWsConnClose(conn, XWS_CLOSE_GOING_AWAY, NULL, 0);
  } else {
    /* Timeout waiting for peer's Close response */
    ws_fire_close(conn, conn->close_code, NULL, 0);
    /* When called directly as a timer callback (not via ws_on_event),
     * we must destroy the connection here since ws_fire_close no
     * longer does it. When called from ws_on_event's Timeout branch,
     * the caller checks close_state and handles destroy via goto. */
    xWsConnDestroy(conn);
  }
}

static void ws_reset_idle_timer(struct xWsConn_ *conn) {
  if (conn->idle_timeout_ms <= 0) return;

  if (conn->idle_timer) {
    xEventLoopTimerCancel(conn->loop, conn->idle_timer);
  }
  conn->idle_timer = xEventLoopTimerAfter(conn->loop, ws_idle_timeout, conn,
                                          (uint64_t)conn->idle_timeout_ms);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Frame processing
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_process_frame(struct xWsConn_ *conn, xWsFrame *frame) {
  uint8_t opcode = frame->opcode;

  switch (opcode) {
  case XWS_OPCODE_PING:
    /* Auto-reply with Pong (same payload) */
    xWsFrameEncode(&conn->write_buf, 1, XWS_OPCODE_PONG, frame->payload,
                   (size_t)frame->payload_len, conn->is_client);
    ws_try_flush(conn);
    break;

  case XWS_OPCODE_PONG:
    /* Unsolicited pong: ignore */
    break;

  case XWS_OPCODE_CLOSE: {
    uint16_t    code       = XWS_CLOSE_NO_STATUS;
    const char *reason     = NULL;
    size_t      reason_len = 0;

    if (frame->payload_len >= 2) {
      code = (uint16_t)((frame->payload[0] << 8) | frame->payload[1]);
      if (frame->payload_len > 2) {
        reason     = (const char *)(frame->payload + 2);
        reason_len = (size_t)frame->payload_len - 2;
      }
    }

    if (conn->close_state == xWsCloseState_Open) {
      /* Peer initiated close: echo the Close frame */
      conn->close_state = xWsCloseState_CloseReceived;
      conn->close_code  = code;
      if (reason_len > 0) {
        conn->close_reason = (char *)malloc(reason_len);
        if (conn->close_reason) {
          memcpy(conn->close_reason, reason, reason_len);
          conn->close_reason_len = reason_len;
        }
      }
      xWsFrameEncodeClose(&conn->write_buf, code, reason, reason_len,
                          conn->is_client);
      ws_try_flush(conn);
      /* Connection will be destroyed after flush completes */
    } else if (conn->close_state == xWsCloseState_CloseSent) {
      /* We sent Close, peer responded: done */
      ws_fire_close(conn, code, reason, reason_len);
    }
    break;
  }

  case XWS_OPCODE_TEXT:
  case XWS_OPCODE_BINARY:
    if (frame->fin) {
      /* Complete single-frame message */
      if (conn->in_fragment) {
        /* Protocol error: new message while in fragment */
        xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
        break;
      }
#ifdef XHTTP_WS_DEFLATE
      if (frame->rsv1 && conn->deflate_ctx) {
        /* Decompress the payload */
        uint8_t *decompressed     = NULL;
        size_t   decompressed_len = 0;
        if (xWsDeflateDecompress(conn->deflate_ctx, frame->payload,
                                 (size_t)frame->payload_len, &decompressed,
                                 &decompressed_len) < 0) {
          xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
          break;
        }
        if (conn->callbacks.on_message) {
          xWsOpcode op =
            (opcode == XWS_OPCODE_TEXT) ? xWsOpcode_Text : xWsOpcode_Binary;
          conn->callbacks.on_message((xWsConn)conn, op, decompressed,
                                     decompressed_len, conn->user_arg);
        }
        free(decompressed);
        break;
      }
#endif
      if (conn->callbacks.on_message) {
        xWsOpcode op =
          (opcode == XWS_OPCODE_TEXT) ? xWsOpcode_Text : xWsOpcode_Binary;
        conn->callbacks.on_message((xWsConn)conn, op, frame->payload,
                                   (size_t)frame->payload_len, conn->user_arg);
      }
    } else {
      /* First fragment of a fragmented message */
      if (conn->in_fragment) {
        xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
        break;
      }
      conn->in_fragment     = 1;
      conn->frag_opcode     = opcode;
      conn->frag_compressed = frame->rsv1 ? 1 : 0;
      xIOBufferReset(&conn->frag_buf);
      if (frame->payload_len > 0) {
        xIOBufferAppend(&conn->frag_buf, frame->payload,
                        (size_t)frame->payload_len);
      }
    }
    break;

  case XWS_OPCODE_CONTINUATION:
    if (!conn->in_fragment) {
      /* Continuation without a starting frame */
      xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
      break;
    }
    if (frame->payload_len > 0) {
      xIOBufferAppend(&conn->frag_buf, frame->payload,
                      (size_t)frame->payload_len);
    }
    if (frame->fin) {
      /* Final fragment: deliver reassembled message */
      conn->in_fragment  = 0;
      size_t   total     = xIOBufferLen(&conn->frag_buf);
      uint8_t *assembled = NULL;
      if (total > 0) {
        assembled = (uint8_t *)malloc(total);
        if (assembled) {
          xIOBufferCopyTo(&conn->frag_buf, assembled);
        }
      }
      xIOBufferReset(&conn->frag_buf);

#ifdef XHTTP_WS_DEFLATE
      /* RFC 7692: RSV1 on the first fragment indicates the
       * entire reassembled message is compressed. We stored
       * the raw (compressed) fragments; now decompress. */
      if (conn->frag_compressed && conn->deflate_ctx) {
        uint8_t *decompressed     = NULL;
        size_t   decompressed_len = 0;
        if (assembled && total > 0 &&
            xWsDeflateDecompress(conn->deflate_ctx, assembled, total,
                                 &decompressed, &decompressed_len) == 0) {
          free(assembled);
          assembled = decompressed;
          total     = decompressed_len;
        } else {
          free(assembled);
          xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
          break;
        }
      }
#endif

      if (conn->callbacks.on_message) {
        xWsOpcode op = (conn->frag_opcode == XWS_OPCODE_TEXT)
                         ? xWsOpcode_Text
                         : xWsOpcode_Binary;
        conn->callbacks.on_message((xWsConn)conn, op, assembled, total,
                                   conn->user_arg);
      }
      free(assembled);
    }
    break;

  default:
    /* Unknown opcode: protocol error */
    xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Socket event handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void ws_on_event(xSocket sock, xEventMask mask, void *arg) {
  struct xWsConn_ *conn = (struct xWsConn_ *)arg;
  (void)sock;

  if (conn->close_state == xWsCloseState_Closed) goto destroy;

  /* Timeout */
  if (mask & xEvent_Timeout) {
    ws_idle_timeout(conn);
    /* ws_idle_timeout handles its own destroy when needed,
     * so we must not fall through to the destroy label. */
    return;
  }

  /* Writable: flush pending data */
  if (mask & xEvent_Write) {
    ws_try_flush(conn);
    if (conn->close_state == xWsCloseState_Closed) goto destroy;
  }

  /* Readable: read data and parse frames.
   *
   * Edge-triggered drain loop: we must read until EAGAIN to ensure
   * no data is left unread (kqueue EV_CLEAR / epoll EPOLLET won't
   * re-notify for data already in the socket buffer).  This is
   * critical for TLS where SSL_read may return one record at a time
   * while the underlying socket has multiple records buffered. */
  if (mask & xEvent_Read) {
    for (;;) {
      xnet_ssize_t n = xIOBufferReadWith(&conn->read_buf, conn->transport.read,
                                         conn->transport.ctx);
      if (n < 0) {
        if (xnet_errno() == XNET_EAGAIN || xnet_errno() == XNET_EWOULDBLOCK) {
          /* No more data available right now; parse what we have
           * and wait for next event. */
          break;
        }
        ws_fire_close(conn, XWS_CLOSE_ABNORMAL, NULL, 0);
        goto destroy;
      }
      if (n == 0) {
        ws_fire_close(conn, XWS_CLOSE_ABNORMAL, NULL, 0);
        goto destroy;
      }
    }

    /* Reset idle timer on data received */
    ws_reset_idle_timer(conn);

    /* Parse frames */
    while (conn->close_state != xWsCloseState_Closed) {
      xWsFrameResult result = xWsFrameParse(&conn->parser, &conn->read_buf);

      if (result == xWsFrameResult_Ok) {
        ws_process_frame(conn, &conn->parser.frame);
        /* Free frame payload and reset parser */
        free(conn->parser.frame.payload);
        conn->parser.frame.payload = NULL;
        xWsFrameParserReset(&conn->parser);
        continue;
      }

      if (result == xWsFrameResult_NeedMore) {
        break;
      }

      /* Protocol error */
      xWsConnClose(conn, XWS_CLOSE_PROTOCOL_ERR, NULL, 0);
      break;
    }
    if (conn->close_state == xWsCloseState_Closed) goto destroy;
  }

  return;

destroy:
  xWsConnDestroy(conn);
}
