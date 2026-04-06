/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_deflate.c - permessage-deflate implementation (RFC 7692)
 *
 * Uses zlib for DEFLATE compression/decompression.
 * Compiled only when XHTTP_WS_DEFLATE is defined.
 */

#ifdef XHTTP_WS_DEFLATE

#include "ws_deflate.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

/* RFC 7692 §7.2.1: tail bytes appended before inflate,
 * stripped after deflate. */
static const uint8_t DEFLATE_TAIL[] = {0x00, 0x00, 0xFF, 0xFF};

/* Initial output buffer size for compress/decompress */
#define DEFLATE_INIT_BUF_SIZE 4096

/* ───────────────── Context structure ───────────────── */

XDEF_STRUCT(xWsDeflateCtx_) {
  z_stream deflate_strm;   /* Compression stream   */
  z_stream inflate_strm;   /* Decompression stream */
  int deflate_init;        /* deflate_strm initialized */
  int inflate_init;        /* inflate_strm initialized */
  int no_context_takeover_send; /* Reset deflate per msg */
  int no_context_takeover_recv; /* Reset inflate per msg */
};

/* ───────────────── Create / Destroy ───────────────── */

xWsDeflateCtx *xWsDeflateCreate(const xWsDeflateParams *params,
                                 int is_client) {
  xWsDeflateCtx *ctx =
    (xWsDeflateCtx *)calloc(1, sizeof(xWsDeflateCtx));
  if (!ctx) return NULL;

  /* Determine window bits for each direction.
   * Client sends with client_max_window_bits,
   * Server sends with server_max_window_bits.
   * Negative windowBits = raw deflate (no zlib header). */
  int send_wbits, recv_wbits;
  if (is_client) {
    send_wbits = params->client_max_window_bits;
    recv_wbits = params->server_max_window_bits;
    ctx->no_context_takeover_send =
      params->client_no_context_takeover;
    ctx->no_context_takeover_recv =
      params->server_no_context_takeover;
  } else {
    send_wbits = params->server_max_window_bits;
    recv_wbits = params->client_max_window_bits;
    ctx->no_context_takeover_send =
      params->server_no_context_takeover;
    ctx->no_context_takeover_recv =
      params->client_no_context_takeover;
  }

  /* Initialize deflate (compression) stream.
   * Use negative windowBits for raw deflate. */
  ctx->deflate_strm.zalloc = Z_NULL;
  ctx->deflate_strm.zfree  = Z_NULL;
  ctx->deflate_strm.opaque = Z_NULL;
  if (deflateInit2(&ctx->deflate_strm, Z_DEFAULT_COMPRESSION,
                   Z_DEFLATED, -send_wbits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    free(ctx);
    return NULL;
  }
  ctx->deflate_init = 1;

  /* Initialize inflate (decompression) stream.
   * Use negative windowBits for raw inflate. */
  ctx->inflate_strm.zalloc = Z_NULL;
  ctx->inflate_strm.zfree  = Z_NULL;
  ctx->inflate_strm.opaque = Z_NULL;
  if (inflateInit2(&ctx->inflate_strm, -recv_wbits) != Z_OK) {
    deflateEnd(&ctx->deflate_strm);
    free(ctx);
    return NULL;
  }
  ctx->inflate_init = 1;

  return ctx;
}

void xWsDeflateDestroy(xWsDeflateCtx *ctx) {
  if (!ctx) return;
  if (ctx->deflate_init) deflateEnd(&ctx->deflate_strm);
  if (ctx->inflate_init) inflateEnd(&ctx->inflate_strm);
  free(ctx);
}

/* ───────────────── Compress ───────────────── */

int xWsDeflateCompress(xWsDeflateCtx *ctx,
                       const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len) {
  if (!ctx || !out || !out_len) return -1;

  /* Reset stream if no_context_takeover */
  if (ctx->no_context_takeover_send) {
    if (deflateReset(&ctx->deflate_strm) != Z_OK)
      return -1;
  }

  /* Allocate output buffer (compressed is usually smaller,
   * but may be larger for tiny inputs) */
  size_t buf_sz = (in_len > 0)
    ? (in_len + in_len / 10 + 64)
    : DEFLATE_INIT_BUF_SIZE;
  uint8_t *buf = (uint8_t *)malloc(buf_sz);
  if (!buf) return -1;

  ctx->deflate_strm.next_in  = (Bytef *)(uintptr_t)in;
  ctx->deflate_strm.avail_in = (uInt)in_len;
  ctx->deflate_strm.next_out  = buf;
  ctx->deflate_strm.avail_out = (uInt)buf_sz;

  /* Flush with Z_SYNC_FLUSH to produce the 0x00 0x00 0xFF 0xFF
   * tail that we will then strip. */
  int rc = deflate(&ctx->deflate_strm, Z_SYNC_FLUSH);
  if (rc != Z_OK && rc != Z_BUF_ERROR) {
    free(buf);
    return -1;
  }

  /* If output buffer was too small, grow and retry */
  while (ctx->deflate_strm.avail_out == 0) {
    size_t written = buf_sz;
    buf_sz *= 2;
    uint8_t *tmp = (uint8_t *)realloc(buf, buf_sz);
    if (!tmp) { free(buf); return -1; }
    buf = tmp;
    ctx->deflate_strm.next_out  = buf + written;
    ctx->deflate_strm.avail_out = (uInt)(buf_sz - written);
    rc = deflate(&ctx->deflate_strm, Z_SYNC_FLUSH);
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
      free(buf);
      return -1;
    }
  }

  size_t total = buf_sz - ctx->deflate_strm.avail_out;

  /* Strip trailing 0x00 0x00 0xFF 0xFF (RFC 7692 §7.2.1) */
  if (total >= 4 &&
      memcmp(buf + total - 4, DEFLATE_TAIL, 4) == 0) {
    total -= 4;
  }

  *out = buf;
  *out_len = total;
  return 0;
}

/* ───────────────── Decompress ───────────────── */

int xWsDeflateDecompress(xWsDeflateCtx *ctx,
                         const uint8_t *in, size_t in_len,
                         uint8_t **out, size_t *out_len) {
  if (!ctx || !out || !out_len) return -1;

  /* Reset stream if no_context_takeover */
  if (ctx->no_context_takeover_recv) {
    if (inflateReset(&ctx->inflate_strm) != Z_OK)
      return -1;
  }

  /* Build input: compressed data + tail bytes */
  size_t full_len = in_len + sizeof(DEFLATE_TAIL);
  uint8_t *full_in = (uint8_t *)malloc(full_len);
  if (!full_in) return -1;
  if (in_len > 0) memcpy(full_in, in, in_len);
  memcpy(full_in + in_len, DEFLATE_TAIL, sizeof(DEFLATE_TAIL));

  /* Allocate output buffer */
  size_t buf_sz = DEFLATE_INIT_BUF_SIZE;
  if (in_len * 4 > buf_sz) buf_sz = in_len * 4;
  uint8_t *buf = (uint8_t *)malloc(buf_sz);
  if (!buf) { free(full_in); return -1; }

  ctx->inflate_strm.next_in  = full_in;
  ctx->inflate_strm.avail_in = (uInt)full_len;
  ctx->inflate_strm.next_out  = buf;
  ctx->inflate_strm.avail_out = (uInt)buf_sz;

  int rc = inflate(&ctx->inflate_strm, Z_SYNC_FLUSH);

  /* Grow buffer and continue if needed */
  while (rc == Z_OK && ctx->inflate_strm.avail_in > 0) {
    size_t written = buf_sz - ctx->inflate_strm.avail_out;
    buf_sz *= 2;
    /* Limit decompressed size to 64 MB to prevent zip bombs */
    if (buf_sz > 64 * 1024 * 1024) {
      free(buf);
      free(full_in);
      return -1;
    }
    uint8_t *tmp = (uint8_t *)realloc(buf, buf_sz);
    if (!tmp) { free(buf); free(full_in); return -1; }
    buf = tmp;
    ctx->inflate_strm.next_out  = buf + written;
    ctx->inflate_strm.avail_out = (uInt)(buf_sz - written);
    rc = inflate(&ctx->inflate_strm, Z_SYNC_FLUSH);
  }

  if (rc != Z_OK && rc != Z_BUF_ERROR) {
    free(buf);
    free(full_in);
    return -1;
  }

  size_t total = buf_sz - ctx->inflate_strm.avail_out;
  free(full_in);

  *out = buf;
  *out_len = total;
  return 0;
}

/* ───────────────── Header parsing ───────────────── */

/**
 * Skip whitespace in a string.
 */
static const char *skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  return p;
}

/**
 * Check if a parameter name matches (case-insensitive).
 */
static int param_match(const char *p, size_t len,
                       const char *name) {
  return len == strlen(name) &&
         strncasecmp(p, name, len) == 0;
}

int xWsDeflateParseOffer(const char *value, size_t value_len,
                         xWsDeflateParams *params) {
  if (!value || !params) return -1;

  memset(params, 0, sizeof(*params));
  /* Defaults per RFC 7692 */
  params->server_max_window_bits = 15;
  params->client_max_window_bits = 15;

  const char *p = value;
  const char *end = value + value_len;

  /* Find "permessage-deflate" token in possibly
   * comma-separated extension list */
  int found = 0;
  while (p < end) {
    p = skip_ws(p, end);
    if (p >= end) break;

    /* Find end of this extension (next comma or end) */
    const char *ext_end = (const char *)memchr(p, ',',
                                               (size_t)(end - p));
    if (!ext_end) ext_end = end;

    /* Check if extension name is "permessage-deflate" */
    const char *semi = (const char *)memchr(p, ';',
                                            (size_t)(ext_end - p));
    const char *name_end = semi ? semi : ext_end;

    /* Trim trailing whitespace from name */
    const char *ne = name_end;
    while (ne > p && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;

    size_t name_len = (size_t)(ne - p);
    if (name_len == 18 &&
        strncasecmp(p, "permessage-deflate", 18) == 0) {
      found = 1;
      params->enabled = 1;

      /* Parse parameters after the semicolons */
      const char *pp = semi ? semi + 1 : ext_end;
      while (pp < ext_end) {
        pp = skip_ws(pp, ext_end);
        if (pp >= ext_end) break;

        /* Find next semicolon or end */
        const char *next_semi =
          (const char *)memchr(pp, ';',
                               (size_t)(ext_end - pp));
        const char *param_end = next_semi ? next_semi : ext_end;

        /* Find '=' if present */
        const char *eq =
          (const char *)memchr(pp, '=',
                               (size_t)(param_end - pp));

        const char *pname_end = eq ? eq : param_end;
        /* Trim trailing ws from param name */
        const char *pne = pname_end;
        while (pne > pp && (pne[-1] == ' ' || pne[-1] == '\t'))
          pne--;
        size_t pname_len = (size_t)(pne - pp);

        if (param_match(pp, pname_len,
                        "server_no_context_takeover")) {
          params->server_no_context_takeover = 1;
        } else if (param_match(pp, pname_len,
                               "client_no_context_takeover")) {
          params->client_no_context_takeover = 1;
        } else if (param_match(pp, pname_len,
                               "server_max_window_bits")) {
          if (eq && eq + 1 < param_end) {
            const char *vp = skip_ws(eq + 1, param_end);
            int bits = 0;
            while (vp < param_end && *vp >= '0' && *vp <= '9') {
              bits = bits * 10 + (*vp - '0');
              vp++;
            }
            if (bits >= 8 && bits <= 15)
              params->server_max_window_bits = bits;
          }
        } else if (param_match(pp, pname_len,
                               "client_max_window_bits")) {
          if (eq && eq + 1 < param_end) {
            const char *vp = skip_ws(eq + 1, param_end);
            int bits = 0;
            while (vp < param_end && *vp >= '0' && *vp <= '9') {
              bits = bits * 10 + (*vp - '0');
              vp++;
            }
            if (bits >= 8 && bits <= 15)
              params->client_max_window_bits = bits;
          } else {
            /* client_max_window_bits without value = client
             * supports it, server may choose */
          }
        }

        pp = next_semi ? next_semi + 1 : ext_end;
      }
      break; /* Use first matching extension */
    }

    p = ext_end;
    if (p < end && *p == ',') p++;
  }

  return found ? 0 : -1;
}

/* ───────────────── Header building ───────────────── */

int xWsDeflateBuildClientOffer(char *buf, size_t buf_sz) {
  /* Offer permessage-deflate with client_max_window_bits
   * (no value = let server decide) */
  return snprintf(buf, buf_sz,
    "permessage-deflate; client_max_window_bits");
}

int xWsDeflateBuildServerResponse(const xWsDeflateParams *params,
                                  char *buf, size_t buf_sz) {
  if (!params || !params->enabled) return -1;

  int n = snprintf(buf, buf_sz, "permessage-deflate");
  if (n < 0 || (size_t)n >= buf_sz) return -1;

  if (params->server_no_context_takeover) {
    int r = snprintf(buf + n, buf_sz - (size_t)n,
                     "; server_no_context_takeover");
    if (r < 0) return -1;
    n += r;
  }
  if (params->client_no_context_takeover) {
    int r = snprintf(buf + n, buf_sz - (size_t)n,
                     "; client_no_context_takeover");
    if (r < 0) return -1;
    n += r;
  }
  if (params->server_max_window_bits < 15) {
    int r = snprintf(buf + n, buf_sz - (size_t)n,
                     "; server_max_window_bits=%d",
                     params->server_max_window_bits);
    if (r < 0) return -1;
    n += r;
  }
  if (params->client_max_window_bits < 15) {
    int r = snprintf(buf + n, buf_sz - (size_t)n,
                     "; client_max_window_bits=%d",
                     params->client_max_window_bits);
    if (r < 0) return -1;
    n += r;
  }

  return n;
}

#endif /* XHTTP_WS_DEFLATE */
