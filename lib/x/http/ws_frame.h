/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_frame.h - WebSocket frame codec (RFC 6455 §5)
 *
 * Provides incremental frame parsing from an xIOBuffer and frame
 * encoding for server-to-client transmission.
 */

#ifndef XHTTP_WS_FRAME_H
#define XHTTP_WS_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <x/base/base.h>
#include <x/buf/io.h>

/* ───────────────────── Opcodes (RFC 6455 §5.2) ───────────────────── */

#define XWS_OPCODE_CONTINUATION 0x0
#define XWS_OPCODE_TEXT         0x1
#define XWS_OPCODE_BINARY       0x2
#define XWS_OPCODE_CLOSE        0x8
#define XWS_OPCODE_PING         0x9
#define XWS_OPCODE_PONG         0xA

/* ───────────────────── Close status codes ───────────────────── */

#define XWS_CLOSE_NORMAL        1000
#define XWS_CLOSE_GOING_AWAY    1001
#define XWS_CLOSE_PROTOCOL_ERR  1002
#define XWS_CLOSE_UNSUPPORTED   1003
#define XWS_CLOSE_NO_STATUS     1005
#define XWS_CLOSE_ABNORMAL      1006

/* ───────────────────── Parse result ───────────────────── */

/**
 * Return values for xWsFrameParse().
 */
XDEF_ENUM(xWsFrameResult){
  /** A complete frame was parsed and consumed from the buffer. */
  xWsFrameResult_Ok = 0,
  /** Not enough data in the buffer; call again after more I/O. */
  xWsFrameResult_NeedMore = 1,
  /** Protocol error detected (caller should send Close 1002). */
  xWsFrameResult_Error = -1,
};

/* ───────────────────── Frame structure ───────────────────── */

/**
 * Represents a single parsed WebSocket frame.
 *
 * After a successful xWsFrameParse(), the payload points to a
 * heap-allocated buffer that the caller must free().
 */
XDEF_STRUCT(xWsFrame) {
  uint8_t  fin;            /**< FIN bit (1 = final fragment)     */
  uint8_t  rsv1;           /**< RSV1 bit (1 = compressed, PMD)   */
  uint8_t  opcode;         /**< Frame opcode (4 bits)            */
  uint8_t  masked;         /**< MASK bit                         */
  uint8_t  masking_key[4]; /**< Masking key (if masked)          */
  uint64_t payload_len;    /**< Payload length in bytes          */
  uint8_t *payload;        /**< Payload data (heap, caller frees)*/
};

/* ───────────────────── Parse state machine ───────────────────── */

/**
 * Internal parsing phase for the frame parser.
 */
XDEF_ENUM(xWsFrameParserPhase){
  xWsFrameParserPhase_Header  = 0, /**< Reading 2-byte base header    */
  xWsFrameParserPhase_Len16,       /**< Reading 2-byte extended length*/
  xWsFrameParserPhase_Len64,       /**< Reading 8-byte extended length*/
  xWsFrameParserPhase_Mask,        /**< Reading 4-byte masking key    */
  xWsFrameParserPhase_Payload,     /**< Reading payload data          */
};

/**
 * Incremental frame parser state.
 *
 * Maintains parsing progress across multiple I/O reads.
 * Initialize with xWsFrameParserInit() before first use.
 */
XDEF_STRUCT(xWsFrameParser) {
  xWsFrameParserPhase phase;       /**< Internal parsing phase        */

  xWsFrame frame;           /**< Frame being assembled            */
  size_t   payload_read;    /**< Bytes of payload read so far     */

  /**
   * Whether incoming frames are expected to be masked.
   * Server mode (expect_masked=1): reject unmasked frames.
   * Client mode (expect_masked=0): reject masked frames.
   */
  int expect_masked;

  /**
   * Whether RSV1 bit is allowed (permessage-deflate).
   * When 0, any RSV bit set causes a protocol error.
   * When 1, RSV1 is permitted on data frames.
   */
  int allow_rsv1;
};

/* ───────────────────── API ───────────────────── */

/**
 * Initialize a frame parser for first use.
 *
 * @param parser         Parser state to initialize.
 * @param expect_masked  1 for server mode (expect masked frames),
 *                       0 for client mode (expect unmasked).
 */
void xWsFrameParserInit(xWsFrameParser *parser,
                        int expect_masked);

/**
 * Reset the parser for the next frame (after a successful parse).
 *
 * Does NOT free the previous frame's payload — the caller owns it.
 *
 * @param parser  Parser state to reset.
 */
void xWsFrameParserReset(xWsFrameParser *parser);

/**
 * Attempt to parse a complete frame from the I/O buffer.
 *
 * On xWsFrameResult_Ok, the parsed frame is available in
 * parser->frame. The consumed bytes are removed from @p io.
 * The caller is responsible for freeing parser->frame.payload.
 *
 * On xWsFrameResult_NeedMore, no data is consumed; the caller
 * should read more data and call again.
 *
 * On xWsFrameResult_Error, a protocol violation was detected.
 * The caller should send a Close frame with status 1002.
 *
 * @param parser  Parser state (must be initialized).
 * @param io      I/O buffer containing incoming data.
 * @return Parse result.
 */
xWsFrameResult xWsFrameParse(xWsFrameParser *parser, xIOBuffer *io);

/**
 * Encode a WebSocket frame and append it to the I/O buffer.
 *
 * When @p masked is non-zero, a random 4-byte masking key is
 * generated and the payload is XOR-encoded per RFC 6455 §5.3.
 * Client frames MUST be masked; server frames MUST NOT.
 *
 * @param io           Output I/O buffer.
 * @param fin          FIN bit (1 for non-fragmented or final fragment).
 * @param opcode       Frame opcode.
 * @param payload      Payload data, or NULL if payload_len == 0.
 * @param payload_len  Payload length in bytes.
 * @param masked       Non-zero to mask the frame (client mode).
 * @return 0 on success, -1 on error (OOM).
 */
int xWsFrameEncode(xIOBuffer *io, uint8_t fin, uint8_t opcode,
                   const void *payload, size_t payload_len,
                   int masked);

/**
 * Encode a WebSocket frame with RSV1 bit set (compressed).
 *
 * Same as xWsFrameEncode but sets the RSV1 bit in the header
 * for permessage-deflate compressed messages.
 *
 * @param io           Output I/O buffer.
 * @param fin          FIN bit.
 * @param rsv1         RSV1 bit (1 for compressed).
 * @param opcode       Frame opcode.
 * @param payload      Payload data.
 * @param payload_len  Payload length in bytes.
 * @param masked       Non-zero to mask the frame.
 * @return 0 on success, -1 on error.
 */
int xWsFrameEncodeEx(xIOBuffer *io, uint8_t fin, uint8_t rsv1,
                     uint8_t opcode, const void *payload,
                     size_t payload_len, int masked);

/**
 * Encode a Close frame with a status code and optional reason.
 *
 * @param io      Output I/O buffer.
 * @param code    Close status code (network byte order handled
 *                internally).
 * @param reason  Optional reason string, or NULL.
 * @param len     Length of reason in bytes.
 * @param masked  Non-zero to mask the frame (client mode).
 * @return 0 on success, -1 on error.
 */
int xWsFrameEncodeClose(xIOBuffer *io, uint16_t code,
                        const char *reason, size_t len,
                        int masked);

#endif /* XHTTP_WS_FRAME_H */
