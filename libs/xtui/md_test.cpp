/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md_test.cpp - P0 contract tests for xMd.
 *
 * Tests operate on the sink output as a plain std::string. Each
 * expected string embeds raw \x1b escapes to make the SGR
 * transitions explicit - easier to debug than hex diffs.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

extern "C" {
#include <xtui/md.h>
}

namespace {

/* Collecting sink used by every test. */
void CollectSink(const char *data, size_t len, void *arg) {
  auto *out = static_cast<std::string *>(arg);
  out->append(data, len);
}

/* Convenience wrapper: feed the whole payload in one chunk + flush,
 * return the accumulated sink bytes. */
std::string Render(const std::string &md) {
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);
  xMdFeed(&r, md.data(), md.size());
  xMdFlush(&r);
  return out;
}

/* Feed the payload one byte at a time to exercise cross-chunk
 * pending-buffer handling. Should produce identical output to the
 * whole-payload Render() for any valid markdown. */
std::string RenderByteByByte(const std::string &md) {
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);
  for (char c : md) xMdFeed(&r, &c, 1);
  xMdFlush(&r);
  return out;
}

} /* namespace */

/* ========== Inline emphasis ========== */

TEST(MdTest, BoldWholeChunk) {
  EXPECT_EQ(Render("**hi**"), "\x1b[1mhi\x1b[22m");
}

TEST(MdTest, BoldAcrossChunks) {
  /* This is the whole point: "**" must survive a chunk boundary. */
  EXPECT_EQ(RenderByteByByte("**hi**"), "\x1b[1mhi\x1b[22m");
}

TEST(MdTest, ItalicStar) {
  EXPECT_EQ(Render("*hi*"), "\x1b[3mhi\x1b[23m");
}

TEST(MdTest, ItalicUnderscore) {
  EXPECT_EQ(Render("_hi_"), "\x1b[3mhi\x1b[23m");
}

TEST(MdTest, UnderscoreInSnakeCase) {
  /* Must not flip italic - "foo_bar" is a literal identifier. */
  EXPECT_EQ(Render("foo_bar"), "foo_bar");
}

TEST(MdTest, InlineCode) {
  EXPECT_EQ(Render("`ls`"), "\x1b[4mls\x1b[24m");
}

/* ========== Bullet-list defusing ========== */

TEST(MdTest, BulletAtBolStaysLiteral) {
  /* "* item" at bol is a list marker; we don't render lists at P0,
   * but we must NOT open an unclosed italic span. */
  EXPECT_EQ(Render("* item\n"), "* item\n");
}

/* ========== Fenced code block ========== */

TEST(MdTest, FencedCodePassesThrough) {
  /* Inside the fence, '*' stays literal - this is the single most
   * important invariant. */
  std::string in = "```\nfor i in *; do\n```\n";
  EXPECT_EQ(Render(in), in);
}

TEST(MdTest, FencedCodeAcrossChunks) {
  std::string in = "```\nfor i in *; do\n```\n";
  EXPECT_EQ(RenderByteByByte(in), in);
}

/* ========== Headings ========== */

TEST(MdTest, HeadingH1) {
  EXPECT_EQ(Render("# Title\n"), "# \x1b[1mTitle\x1b[22m\n");
}

TEST(MdTest, HashInProseStaysLiteral) {
  /* "#" not at bol is prose (e.g. "PR #123"). */
  EXPECT_EQ(Render("PR #123\n"), "PR #123\n");
}

/* ========== Flush / Reset ========== */

TEST(MdTest, FlushClosesOpenSpans) {
  /* Unclosed bold at EOF must trigger an SGR reset so the next
   * chrome renders cleanly. */
  std::string s = Render("**oops");
  /* Must contain a reset at the end. */
  EXPECT_NE(s.find("\x1b[0m"), std::string::npos);
}

TEST(MdTest, ResetIsIdempotent) {
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);
  xMdFeed(&r, "**hi", 4);
  xMdReset(&r);
  size_t after_first = out.size();
  xMdReset(&r);
  /* Second reset must not emit anything (no open spans). */
  EXPECT_EQ(out.size(), after_first);
}

TEST(MdTest, FlushOnEmptyStream) {
  /* xMdFlush on an untouched renderer must be a no-op. */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);
  xMdFlush(&r);
  EXPECT_EQ(out, "");
}

/* ========== Escape ========== */

TEST(MdTest, BackslashEscapesAsterisk) {
  /* "\*" must emit a literal '*', not open italic. */
  EXPECT_EQ(Render("\\*hi\\*"), "*hi*");
}

/* ========== UTF-8 cross-chunk ========== */

TEST(MdTest, Utf8ThreeByteAcrossChunks) {
  /* 你 = U+4F60 = 0xE4 0xBD 0xA0 in UTF-8.
   * Feed leading byte in one chunk, continuation bytes in the next.
   * Without UTF-8 pending, the 0xE4 would be emitted as a lone byte
   * (rendered as '?' or '�') before the rest arrives. */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  /* Chunk 1: just the leading byte. */
  const char c1[] = {(char)0xE4};
  xMdFeed(&r, c1, 1);
  EXPECT_EQ(out, "") << "leading byte must be buffered, not emitted";

  /* Chunk 2: continuation bytes. */
  const char c2[] = {(char)0xBD, (char)0xA0};
  xMdFeed(&r, c2, 2);
  EXPECT_EQ(out, "\xE4\xBD\xA0") << "full character emitted once complete";

  xMdFlush(&r);
}

TEST(MdTest, Utf8ThreeByteSplitAfterSecond) {
  /* 你 split after second byte: [0xE4, 0xBD] then [0xA0]. */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  const char c1[] = {(char)0xE4, (char)0xBD};
  xMdFeed(&r, c1, 2);
  EXPECT_EQ(out, "") << "partial sequence must be buffered";

  const char c2[] = {(char)0xA0};
  xMdFeed(&r, c2, 1);
  EXPECT_EQ(out, "\xE4\xBD\xA0") << "full character emitted once complete";

  xMdFlush(&r);
}

TEST(MdTest, Utf8MixedContentAcrossChunks) {
  /* "hi你" split as "hi" + "你" (0xE4 0xBD 0xA0). */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  xMdFeed(&r, "hi", 2);
  EXPECT_EQ(out, "hi");

  const char c2[] = {(char)0xE4, (char)0xBD, (char)0xA0};
  xMdFeed(&r, c2, 3);
  EXPECT_EQ(out, "hi\xE4\xBD\xA0");

  xMdFlush(&r);
}

TEST(MdTest, Utf8AcrossChunksByteByByte) {
  /* 你 byte-by-byte: same as RenderByteByByte but for a Chinese char. */
  std::string chi = "\xE4\xBD\xA0";  /* 你 */
  EXPECT_EQ(Render(chi), chi) << "whole-chunk baseline";

  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);
  for (char c : chi) xMdFeed(&r, &c, 1);
  xMdFlush(&r);
  EXPECT_EQ(out, chi) << "byte-by-byte must reconstruct the same character";
}

TEST(MdTest, Utf8FourByteAcrossChunks) {
  /* 𝄞 (U+1D11E, Musical Symbol G Clef) = F0 9D 84 9E.
   * Split as [F0 9D] then [84 9E]. */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  const char c1[] = {(char)0xF0, (char)0x9D};
  xMdFeed(&r, c1, 2);
  EXPECT_EQ(out, "");

  const char c2[] = {(char)0x84, (char)0x9E};
  xMdFeed(&r, c2, 2);
  EXPECT_EQ(out, "\xF0\x9D\x84\x9E");

  xMdFlush(&r);
}

TEST(MdTest, Utf8PendingDrainedOnFlush) {
  /* A lone leading byte at end-of-stream should be emitted on Flush
   * (it's an incomplete but valid-ish sequence; the caller's stream
   * ended so we can't wait for more bytes). */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  const char c1[] = {(char)0xE4};
  xMdFeed(&r, c1, 1);
  EXPECT_EQ(out, "");

  xMdFlush(&r);
  /* Flush should emit the stashed byte as-is (incomplete UTF-8). */
  EXPECT_EQ(out.size(), (size_t)1);
  EXPECT_EQ(out[0], (char)0xE4);
}

TEST(MdTest, Utf8ResetClearsPending) {
  /* xMdReset must discard any pending UTF-8 bytes. */
  std::string out;
  xMd         r;
  xMdInit(&r, CollectSink, &out);

  const char c1[] = {(char)0xE4};
  xMdFeed(&r, c1, 1);
  EXPECT_EQ(out, "");

  xMdReset(&r);

  /* After reset, feeding a continuation byte should NOT reconstruct
   * the character — the pending leading byte was discarded. */
  const char c2[] = {(char)0xBD, (char)0xA0};
  xMdFeed(&r, c2, 2);
  /* These are orphan continuation bytes; they fall through the
   * default path and get emitted individually. */
  EXPECT_EQ(out.size(), (size_t)2);

  xMdFlush(&r);
}
