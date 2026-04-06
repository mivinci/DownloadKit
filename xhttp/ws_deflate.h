/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_deflate.h - permessage-deflate extension (RFC 7692)
 *
 * Provides per-connection compression/decompression contexts
 * using zlib. Compiled only when XHTTP_WS_DEFLATE is defined.
 */

#ifndef XHTTP_WS_DEFLATE_H
#define XHTTP_WS_DEFLATE_H

#ifdef XHTTP_WS_DEFLATE

#include <stddef.h>
#include <stdint.h>
#include <xbase/base.h>

/* ───────────────── Negotiated parameters ───────────────── */

/**
 * Negotiated permessage-deflate parameters.
 *
 * Populated during the WebSocket handshake and used to
 * configure the zlib streams.
 */
XDEF_STRUCT(xWsDeflateParams) {
  int enabled;                  /**< Extension negotiated     */
  int server_no_context_takeover; /**< Server resets deflate  */
  int client_no_context_takeover; /**< Client resets deflate  */
  int server_max_window_bits;   /**< Server LZ77 window size  */
  int client_max_window_bits;   /**< Client LZ77 window size  */
};

/* ───────────────── Deflate context ───────────────── */

/**
 * Opaque deflate context (wraps two zlib streams).
 */
typedef struct xWsDeflateCtx_ xWsDeflateCtx;

/**
 * Create a deflate context from negotiated parameters.
 *
 * @param params  Negotiated parameters.
 * @param is_client  Non-zero for client side.
 * @return Context, or NULL on failure.
 */
xWsDeflateCtx *xWsDeflateCreate(const xWsDeflateParams *params,
                                 int is_client);

/**
 * Destroy a deflate context and free zlib resources.
 *
 * @param ctx  Context to destroy (may be NULL).
 */
void xWsDeflateDestroy(xWsDeflateCtx *ctx);

/**
 * Compress a message payload.
 *
 * Allocates the output buffer; caller must free() it.
 * Per RFC 7692, the trailing 0x00 0x00 0xFF 0xFF is
 * stripped from the compressed output.
 *
 * @param ctx       Deflate context.
 * @param in        Input payload.
 * @param in_len    Input length.
 * @param out       Receives compressed data (heap).
 * @param out_len   Receives compressed length.
 * @return 0 on success, -1 on error.
 */
int xWsDeflateCompress(xWsDeflateCtx *ctx,
                       const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len);

/**
 * Decompress a message payload.
 *
 * Allocates the output buffer; caller must free() it.
 * Per RFC 7692, appends 0x00 0x00 0xFF 0xFF before
 * inflating.
 *
 * @param ctx       Deflate context.
 * @param in        Compressed payload.
 * @param in_len    Compressed length.
 * @param out       Receives decompressed data (heap).
 * @param out_len   Receives decompressed length.
 * @return 0 on success, -1 on error.
 */
int xWsDeflateDecompress(xWsDeflateCtx *ctx,
                         const uint8_t *in, size_t in_len,
                         uint8_t **out, size_t *out_len);

/**
 * Parse a Sec-WebSocket-Extensions header value and populate
 * deflate parameters.
 *
 * @param value     Header value string.
 * @param value_len Length of value.
 * @param params    Output parameters.
 * @return 0 if permessage-deflate was found, -1 otherwise.
 */
int xWsDeflateParseOffer(const char *value, size_t value_len,
                         xWsDeflateParams *params);

/**
 * Build a Sec-WebSocket-Extensions header value for a client
 * offer.
 *
 * @param buf      Output buffer.
 * @param buf_sz   Buffer size.
 * @return Number of bytes written (excluding NUL), or -1.
 */
int xWsDeflateBuildClientOffer(char *buf, size_t buf_sz);

/**
 * Build a Sec-WebSocket-Extensions header value for a server
 * response, based on the negotiated parameters.
 *
 * @param params   Negotiated parameters.
 * @param buf      Output buffer.
 * @param buf_sz   Buffer size.
 * @return Number of bytes written (excluding NUL), or -1.
 */
int xWsDeflateBuildServerResponse(const xWsDeflateParams *params,
                                  char *buf, size_t buf_sz);

#endif /* XHTTP_WS_DEFLATE */
#endif /* XHTTP_WS_DEFLATE_H */
