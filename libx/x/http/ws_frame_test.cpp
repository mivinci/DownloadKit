/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_frame_test.cpp - WebSocket frame masking unit tests
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "ws_frame.h"
#include <x/buf/io.h>
}

/* ═══════════════════════════════════════════════════════════════════
 *  Helper: encode a frame, then parse it back
 * ═══════════════════════════════════════════════════════════════════
 */

static void encode_then_parse(const char *payload,
                              size_t len,
                              int masked,
                              int expect_masked) {
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT,
                          payload, len, masked);
  ASSERT_EQ(rc, 0);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, expect_masked);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);

  EXPECT_EQ(parser.frame.fin, 1);
  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_TEXT);
  EXPECT_EQ(parser.frame.masked, masked ? 1 : 0);
  EXPECT_EQ(parser.frame.payload_len, (uint64_t)len);

  if (len > 0) {
    ASSERT_NE(parser.frame.payload, nullptr);
    EXPECT_EQ(memcmp(parser.frame.payload, payload, len), 0);
  }

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests: masked encode → decode round-trip
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(WsFrameMask, MaskedEncodeDecodeSmall) {
  /* Small payload: "Hello" */
  encode_then_parse("Hello", 5, /*masked=*/1,
                    /*expect_masked=*/1);
}

TEST(WsFrameMask, MaskedEncodeDecodeEmpty) {
  /* Empty payload */
  encode_then_parse(nullptr, 0, /*masked=*/1,
                    /*expect_masked=*/1);
}

TEST(WsFrameMask, MaskedEncodeDecodeLarge) {
  /* Large payload: 4096 bytes */
  std::vector<char> data(4096, 'A');
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (char)(i & 0xFF);
  }
  encode_then_parse(data.data(), data.size(), /*masked=*/1,
                    /*expect_masked=*/1);
}

TEST(WsFrameMask, MaskedEncodeDecodeMedium) {
  /* Medium payload: 200 bytes (triggers 16-bit length) */
  std::string data(200, 'X');
  encode_then_parse(data.c_str(), data.size(), /*masked=*/1,
                    /*expect_masked=*/1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests: unmasked encode → decode (server behavior)
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(WsFrameMask, UnmaskedEncodeDecodeSmall) {
  encode_then_parse("World", 5, /*masked=*/0,
                    /*expect_masked=*/0);
}

TEST(WsFrameMask, UnmaskedEncodeDecodeEmpty) {
  encode_then_parse(nullptr, 0, /*masked=*/0,
                    /*expect_masked=*/0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests: parser rejects frames with wrong mask bit
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(WsFrameMask, ServerRejectsUnmaskedFrame) {
  /* Server parser (expect_masked=1) should reject unmasked */
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT,
                          "test", 4, /*masked=*/0);
  ASSERT_EQ(rc, 0);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, /*expect_masked=*/1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  EXPECT_EQ(result, xWsFrameResult_Error);

  xIOBufferDeinit(&io);
}

TEST(WsFrameMask, ClientRejectsMaskedFrame) {
  /* Client parser (expect_masked=0) should reject masked */
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT,
                          "test", 4, /*masked=*/1);
  ASSERT_EQ(rc, 0);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, /*expect_masked=*/0);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  EXPECT_EQ(result, xWsFrameResult_Error);

  xIOBufferDeinit(&io);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests: masked wire format correctness
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(WsFrameMask, MaskedWireFormatHasMaskBit) {
  /* Verify the MASK bit is set in the wire format */
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT,
                          "Hi", 2, /*masked=*/1);
  ASSERT_EQ(rc, 0);

  /* Masked frame: 2-byte header + 4-byte mask key + 2 payload
   * = 8 bytes total */
  size_t total = xIOBufferLen(&io);
  EXPECT_EQ(total, 2u + 4u + 2u);

  uint8_t buf[16];
  xIOBufferRead(&io, buf, total);

  /* byte 0: FIN=1, opcode=TEXT */
  EXPECT_EQ(buf[0], 0x81);
  /* byte 1: MASK=1, len=2 */
  EXPECT_EQ(buf[1], 0x82);

  /* bytes 2-5: masking key (non-deterministic, just check
   * that payload is XOR'd) */
  uint8_t key[4];
  memcpy(key, buf + 2, 4);
  uint8_t decoded[2];
  decoded[0] = buf[6] ^ key[0];
  decoded[1] = buf[7] ^ key[1];
  EXPECT_EQ(decoded[0], 'H');
  EXPECT_EQ(decoded[1], 'i');

  xIOBufferDeinit(&io);
}

TEST(WsFrameMask, UnmaskedWireFormatNoMaskBit) {
  /* Verify the MASK bit is NOT set for unmasked frames */
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncode(&io, 1, XWS_OPCODE_TEXT,
                          "Hi", 2, /*masked=*/0);
  ASSERT_EQ(rc, 0);

  size_t total = xIOBufferLen(&io);
  EXPECT_EQ(total, 2u + 2u); /* no mask key */

  uint8_t buf[8];
  xIOBufferRead(&io, buf, total);

  EXPECT_EQ(buf[0], 0x81);
  EXPECT_EQ(buf[1], 0x02); /* no MASK bit */
  EXPECT_EQ(buf[2], 'H');
  EXPECT_EQ(buf[3], 'i');

  xIOBufferDeinit(&io);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests: masked Close frame round-trip
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(WsFrameMask, MaskedCloseFrame) {
  xIOBuffer io;
  xIOBufferInit(&io);

  int rc = xWsFrameEncodeClose(&io, 1000, "bye", 3,
                               /*masked=*/1);
  ASSERT_EQ(rc, 0);

  xWsFrameParser parser;
  xWsFrameParserInit(&parser, /*expect_masked=*/1);

  xWsFrameResult result = xWsFrameParse(&parser, &io);
  ASSERT_EQ(result, xWsFrameResult_Ok);

  EXPECT_EQ(parser.frame.opcode, XWS_OPCODE_CLOSE);
  EXPECT_EQ(parser.frame.masked, 1);
  /* payload: 2-byte code + 3-byte reason = 5 */
  EXPECT_EQ(parser.frame.payload_len, 5u);

  /* Verify close code (network byte order) */
  uint16_t code = (uint16_t)((parser.frame.payload[0] << 8) |
                              parser.frame.payload[1]);
  EXPECT_EQ(code, 1000);
  EXPECT_EQ(memcmp(parser.frame.payload + 2, "bye", 3), 0);

  free(parser.frame.payload);
  xIOBufferDeinit(&io);
}
