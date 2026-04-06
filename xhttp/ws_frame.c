/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_frame.c - WebSocket frame codec implementation (RFC 6455 §5)
 */

#include "ws_frame.h"

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sys/random.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ───────────────── Random bytes for masking key ───────────────── */

static void ws_random_bytes(uint8_t *buf, size_t len) {
#ifdef __linux__
  /* getrandom(2) is available on Linux 3.17+ */
  (void)getrandom(buf, len, 0);
#else
  /* arc4random_buf is available on macOS / BSD */
  arc4random_buf(buf, len);
#endif
}

/* ───────────────────── Helpers ───────────────────── */

/** Read exactly @p n bytes from the IOBuffer into @p out.
 *  Returns 1 if successful, 0 if not enough data. */
static int io_peek(xIOBuffer *io, void *out, size_t n) {
  if (xIOBufferLen(io) < n) return 0;
  /* Copy without consuming */
  size_t copied = 0;
  for (size_t i = 0; i < io->nrefs && copied < n; i++) {
    xIOBufferRef *ref = &io->refs[i];
    size_t avail = ref->length;
    size_t want  = n - copied;
    size_t take  = (avail < want) ? avail : want;
    memcpy((uint8_t *)out + copied,
           ref->block->data + ref->offset, take);
    copied += take;
  }
  return 1;
}

/** Unmask payload in-place. */
static void unmask_payload(uint8_t *data, size_t len,
                           const uint8_t key[4]) {
  /* Process 4 bytes at a time for performance */
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    data[i + 0] ^= key[0];
    data[i + 1] ^= key[1];
    data[i + 2] ^= key[2];
    data[i + 3] ^= key[3];
  }
  for (; i < len; i++) {
    data[i] ^= key[i & 3];
  }
}

static int is_control_opcode(uint8_t opcode) {
  return opcode >= 0x8;
}

/* ───────────────────── Parser ───────────────────── */

void xWsFrameParserInit(xWsFrameParser *parser,
                        int expect_masked) {
  memset(parser, 0, sizeof(*parser));
  parser->phase = XWS_PARSE_HEADER;
  parser->expect_masked = expect_masked;
}

void xWsFrameParserReset(xWsFrameParser *parser) {
  parser->phase        = XWS_PARSE_HEADER;
  parser->payload_read = 0;
  memset(&parser->frame, 0, sizeof(parser->frame));
}

xWsFrameResult xWsFrameParse(xWsFrameParser *parser,
                              xIOBuffer *io) {
  xWsFrame *f = &parser->frame;

  /* Phase: HEADER — read 2-byte base header */
  if (parser->phase == XWS_PARSE_HEADER) {
    uint8_t hdr[2];
    if (!io_peek(io, hdr, 2)) return xWsFrameResult_NeedMore;

    f->fin    = (hdr[0] >> 7) & 1;
    f->rsv1   = (hdr[0] >> 6) & 1;
    f->opcode = hdr[0] & 0x0F;
    f->masked = (hdr[1] >> 7) & 1;

    /* RSV2 and RSV3 must always be 0 */
    if (hdr[0] & 0x30) return xWsFrameResult_Error;

    /* RSV1 is only allowed when permessage-deflate is active,
     * and only on data frames (not control frames). */
    if (f->rsv1) {
      if (!parser->allow_rsv1 || is_control_opcode(f->opcode))
        return xWsFrameResult_Error;
    }

    /* Validate mask bit against expected mode (RFC 6455 §5.1):
     * Server mode: client frames MUST be masked.
     * Client mode: server frames MUST NOT be masked. */
    if ((int)f->masked != parser->expect_masked)
      return xWsFrameResult_Error;

    uint8_t len7 = hdr[1] & 0x7F;

    if (len7 < 126) {
      f->payload_len = len7;
      xIOBufferConsume(io, 2);
      parser->phase = f->masked ? XWS_PARSE_MASK
                                : XWS_PARSE_PAYLOAD;
    } else if (len7 == 126) {
      xIOBufferConsume(io, 2);
      parser->phase = XWS_PARSE_LEN16;
    } else { /* len7 == 127 */
      xIOBufferConsume(io, 2);
      parser->phase = XWS_PARSE_LEN64;
    }

    /* Control frame validation */
    if (is_control_opcode(f->opcode)) {
      if (!f->fin) return xWsFrameResult_Error;
      /* Control frames with extended length are checked below */
      if (len7 >= 126) return xWsFrameResult_Error;
    }
  }

  /* Phase: LEN16 — read 2-byte extended length */
  if (parser->phase == XWS_PARSE_LEN16) {
    uint8_t buf[2];
    if (!io_peek(io, buf, 2)) return xWsFrameResult_NeedMore;
    f->payload_len = ((uint64_t)buf[0] << 8) | buf[1];
    xIOBufferConsume(io, 2);
    parser->phase = f->masked ? XWS_PARSE_MASK
                              : XWS_PARSE_PAYLOAD;
  }

  /* Phase: LEN64 — read 8-byte extended length */
  if (parser->phase == XWS_PARSE_LEN64) {
    uint8_t buf[8];
    if (!io_peek(io, buf, 8)) return xWsFrameResult_NeedMore;
    f->payload_len = 0;
    for (int i = 0; i < 8; i++) {
      f->payload_len = (f->payload_len << 8) | buf[i];
    }
    /* MSB must be 0 (RFC 6455 §5.2) */
    if (f->payload_len >> 63) return xWsFrameResult_Error;
    xIOBufferConsume(io, 8);
    parser->phase = f->masked ? XWS_PARSE_MASK
                              : XWS_PARSE_PAYLOAD;
  }

  /* Phase: MASK — read 4-byte masking key */
  if (parser->phase == XWS_PARSE_MASK) {
    if (!io_peek(io, f->masking_key, 4))
      return xWsFrameResult_NeedMore;
    xIOBufferConsume(io, 4);
    parser->phase = XWS_PARSE_PAYLOAD;
  }

  /* Phase: PAYLOAD — read payload data */
  if (parser->phase == XWS_PARSE_PAYLOAD) {
    if (f->payload_len == 0) {
      f->payload = NULL;
      goto done;
    }

    /* Allocate payload buffer on first entry */
    if (!f->payload) {
      /* Limit single frame to 16 MB to prevent OOM */
      if (f->payload_len > (16 * 1024 * 1024))
        return xWsFrameResult_Error;
      f->payload = (uint8_t *)malloc((size_t)f->payload_len);
      if (!f->payload) return xWsFrameResult_Error;
      parser->payload_read = 0;
    }

    /* Read as much payload as available */
    size_t remaining = (size_t)f->payload_len
                       - parser->payload_read;
    size_t avail = xIOBufferLen(io);
    size_t take  = (avail < remaining) ? avail : remaining;

    if (take > 0) {
      xIOBufferRead(io, f->payload + parser->payload_read,
                    take);
      parser->payload_read += take;
    }

    if (parser->payload_read < (size_t)f->payload_len)
      return xWsFrameResult_NeedMore;

    /* Unmask if needed */
    if (f->masked) {
      unmask_payload(f->payload, (size_t)f->payload_len,
                     f->masking_key);
    }

    goto done;
  }

  return xWsFrameResult_NeedMore;

done:
  /* Control frame payload must be <= 125 bytes */
  if (is_control_opcode(f->opcode) && f->payload_len > 125) {
    free(f->payload);
    f->payload = NULL;
    return xWsFrameResult_Error;
  }

  return xWsFrameResult_Ok;
}

/* ───────────────────── Encoder ───────────────────── */

int xWsFrameEncode(xIOBuffer *io, uint8_t fin, uint8_t opcode,
                   const void *payload, size_t payload_len,
                   int masked) {
  return xWsFrameEncodeEx(io, fin, 0, opcode, payload,
                          payload_len, masked);
}

int xWsFrameEncodeEx(xIOBuffer *io, uint8_t fin, uint8_t rsv1,
                     uint8_t opcode, const void *payload,
                     size_t payload_len, int masked) {
  /* Build header (max 14 bytes: 10 header + 4 mask key) */
  uint8_t hdr[14];
  size_t  hdr_len = 0;

  hdr[0] = (uint8_t)((fin ? 0x80 : 0x00) |
                     (rsv1 ? 0x40 : 0x00) |
                     (opcode & 0x0F));

  uint8_t mask_bit = masked ? 0x80 : 0x00;

  if (payload_len < 126) {
    hdr[1]  = mask_bit | (uint8_t)payload_len;
    hdr_len = 2;
  } else if (payload_len <= 0xFFFF) {
    hdr[1]  = mask_bit | 126;
    hdr[2]  = (uint8_t)(payload_len >> 8);
    hdr[3]  = (uint8_t)(payload_len);
    hdr_len = 4;
  } else {
    hdr[1] = mask_bit | 127;
    for (int i = 0; i < 8; i++) {
      hdr[2 + i] = (uint8_t)(payload_len >> (56 - i * 8));
    }
    hdr_len = 10;
  }

  /* Append masking key if masked */
  uint8_t masking_key[4];
  if (masked) {
    ws_random_bytes(masking_key, 4);
    memcpy(hdr + hdr_len, masking_key, 4);
    hdr_len += 4;
  }

  if (xIOBufferAppend(io, hdr, hdr_len) != xErrno_Ok)
    return -1;

  if (payload_len > 0 && payload) {
    if (masked) {
      /* XOR-encode payload before appending */
      uint8_t *tmp = (uint8_t *)malloc(payload_len);
      if (!tmp) return -1;
      memcpy(tmp, payload, payload_len);
      unmask_payload(tmp, payload_len, masking_key);
      int rc = (xIOBufferAppend(io, tmp, payload_len)
                != xErrno_Ok) ? -1 : 0;
      free(tmp);
      if (rc < 0) return -1;
    } else {
      if (xIOBufferAppend(io, payload, payload_len)
          != xErrno_Ok)
        return -1;
    }
  }

  return 0;
}

int xWsFrameEncodeClose(xIOBuffer *io, uint16_t code,
                        const char *reason, size_t len,
                        int masked) {
  /* Close frame payload: 2-byte status code + optional reason */
  size_t payload_len = 2 + len;
  uint8_t *payload = (uint8_t *)malloc(payload_len);
  if (!payload) return -1;

  payload[0] = (uint8_t)(code >> 8);
  payload[1] = (uint8_t)(code);
  if (len > 0 && reason) {
    memcpy(payload + 2, reason, len);
  }

  int ret = xWsFrameEncode(io, 1, XWS_OPCODE_CLOSE,
                           payload, payload_len, masked);
  free(payload);
  return ret;
}
