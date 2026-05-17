/*
 * Copyright 2026 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * flag_test.cpp - Tests for xbase/flag.h command-line flag parser
 */

#include <gtest/gtest.h>

#include <x/base/error.h>
#include <x/base/flag.h>

#include <stdlib.h>
#include <string.h>

#include <initializer_list>
#include <vector>

/* ───────────────────── Helpers ───────────────────── */

/* Thin helper that wraps argv and err_out cleanup.             */
struct ParseResult {
  xErrno rc;
  char  *err;

  ~ParseResult() {
    free(err);
  }
};

static ParseResult Parse(xFlagSet set, std::initializer_list<const char *> args) {
  /* argv must be char *const []; copy into a mutable buffer.   */
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const char *a : args)
    argv.push_back(const_cast<char *>(a));
  argv.push_back(nullptr);
  ParseResult r{xErrno_Ok, nullptr};
  r.rc = xFlagParse(set, (int)args.size(), argv.data(), &r.err);
  return r;
}

/* ───────────────────── Lifecycle ───────────────────── */

TEST(Flag, CreateDestroy) {
  xFlagSet set = xFlagSetCreate("prog", "a test");
  ASSERT_NE(set, nullptr);
  xFlagSetDestroy(set);
}

TEST(Flag, CreateNullProg) {
  ASSERT_EQ(xFlagSetCreate(nullptr, nullptr), nullptr);
}

TEST(Flag, DestroyNullIsSafe) {
  xFlagSetDestroy(nullptr);
}

/* ───────────────────── Registration ───────────────────── */

TEST(Flag, DuplicateLongRejected) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  bool     b1  = false;
  bool     b2  = false;
  ASSERT_EQ(xFlagAddBool(set, "verbose", 'v', "", &b1, xFlagAttr_None), xErrno_Ok);
  ASSERT_EQ(xFlagAddBool(set, "verbose", 'V', "", &b2, xFlagAttr_None), xErrno_AlreadyExists);
  xFlagSetDestroy(set);
}

TEST(Flag, DuplicateShortRejected) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  bool     b1  = false;
  bool     b2  = false;
  ASSERT_EQ(xFlagAddBool(set, "alpha", 'x', "", &b1, xFlagAttr_None), xErrno_Ok);
  ASSERT_EQ(xFlagAddBool(set, "beta", 'x', "", &b2, xFlagAttr_None), xErrno_AlreadyExists);
  xFlagSetDestroy(set);
}

TEST(Flag, DefaultsApplied) {
  xFlagSet    set  = xFlagSetCreate("prog", nullptr);
  const char *url  = nullptr;
  int         port = 0;
  int64_t     big  = 0;
  bool        ipv6 = true; /* should be reset to false         */

  ASSERT_EQ(xFlagAddString(set, "url", 'u', "URL", "", &url, "ws://127.0.0.1", xFlagAttr_None),
            xErrno_Ok);
  ASSERT_EQ(xFlagAddInt(set, "port", 'p', "N", "", &port, 8080, xFlagAttr_None), xErrno_Ok);
  ASSERT_EQ(xFlagAddI64(set, "big", 0, "N", "", &big, 12345, xFlagAttr_None), xErrno_Ok);
  ASSERT_EQ(xFlagAddBool(set, "ipv6", '6', "", &ipv6, xFlagAttr_None), xErrno_Ok);

  ASSERT_STREQ(url, "ws://127.0.0.1");
  ASSERT_EQ(port, 8080);
  ASSERT_EQ(big, 12345);
  ASSERT_EQ(ipv6, false);

  xFlagSetDestroy(set);
}

/* ───────────────────── Basic parse ───────────────────── */

TEST(Flag, ParseLongWithSeparateValue) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *url = nullptr;
  xFlagAddString(set, "url", 'u', "URL", "", &url, "def", xFlagAttr_None);

  auto r = Parse(set, {"prog", "--url", "wss://a"});
  ASSERT_EQ(r.rc, xErrno_Ok) << (r.err ? r.err : "");
  ASSERT_STREQ(url, "wss://a");

  xFlagSetDestroy(set);
}

TEST(Flag, ParseLongWithEqualsValue) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *url = nullptr;
  xFlagAddString(set, "url", 0, "URL", "", &url, nullptr, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--url=wss://b"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(url, "wss://b");

  xFlagSetDestroy(set);
}

TEST(Flag, ParseShortWithSeparateValue) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *url = nullptr;
  xFlagAddString(set, "url", 'u', "URL", "", &url, nullptr, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-u", "wss://c"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(url, "wss://c");

  xFlagSetDestroy(set);
}

TEST(Flag, ParseShortGlued) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *url = nullptr;
  xFlagAddString(set, "url", 'u', "URL", "", &url, nullptr, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-uwss://d"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(url, "wss://d");

  xFlagSetDestroy(set);
}

TEST(Flag, ParseBoolPresence) {
  xFlagSet set  = xFlagSetCreate("prog", nullptr);
  bool     ipv6 = false;
  xFlagAddBool(set, "ipv6", '6', "", &ipv6, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--ipv6"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_EQ(ipv6, true);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseBundledShorts) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  bool     a   = false;
  bool     b   = false;
  bool     c   = false;
  xFlagAddBool(set, "alpha", 'a', "", &a, xFlagAttr_None);
  xFlagAddBool(set, "beta", 'b', "", &b, xFlagAttr_None);
  xFlagAddBool(set, "gamma", 'c', "", &c, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-abc"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseBundledShortsWithTrailingArg) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  bool        v   = false;
  const char *f   = nullptr;
  xFlagAddBool(set, "verbose", 'v', "", &v, xFlagAttr_None);
  xFlagAddString(set, "file", 'f', "FILE", "", &f, nullptr, xFlagAttr_None);

  /* -vfmy.txt → -v plus -f my.txt (glued)                      */
  auto r = Parse(set, {"prog", "-vfmy.txt"});
  ASSERT_EQ(r.rc, xErrno_Ok) << (r.err ? r.err : "");
  ASSERT_TRUE(v);
  ASSERT_STREQ(f, "my.txt");

  xFlagSetDestroy(set);
}

/* ───────────────────── Integers / numbers ───────────────────── */

TEST(Flag, ParseIntDecimalHexBin) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  int      a   = 0;
  int      b   = 0;
  int      c   = 0;
  xFlagAddInt(set, "a", 0, "N", "", &a, 0, xFlagAttr_None);
  xFlagAddInt(set, "b", 0, "N", "", &b, 0, xFlagAttr_None);
  xFlagAddInt(set, "c", 0, "N", "", &c, 0, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--a=42", "--b=0x2A", "--c=0b101010"});
  ASSERT_EQ(r.rc, xErrno_Ok) << (r.err ? r.err : "");
  ASSERT_EQ(a, 42);
  ASSERT_EQ(b, 42);
  ASSERT_EQ(c, 42);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseIntRejectsGarbage) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  int      a   = 0;
  xFlagAddInt(set, "a", 0, "N", "", &a, 0, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--a=12x"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);
  ASSERT_NE(r.err, nullptr);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseU64) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  uint64_t v   = 0;
  xFlagAddU64(set, "size", 's', "BYTES", "", &v, 0, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--size=18446744073709551615"}); /* UINT64_MAX */
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_EQ(v, (uint64_t)-1);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseU64RejectsNegative) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  uint64_t v   = 0;
  xFlagAddU64(set, "size", 's', "BYTES", "", &v, 0, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-s", "-1"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseDouble) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  double   d   = 0.0;
  xFlagAddDouble(set, "r", 0, "R", "", &d, 0.0, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--r=1.5"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_DOUBLE_EQ(d, 1.5);

  xFlagSetDestroy(set);
}

/* ───────────────────── Choice / counter ───────────────────── */

TEST(Flag, ParseChoiceValid) {
  xFlagSet    set       = xFlagSetCreate("prog", nullptr);
  const char *choices[] = {"tcp", "udp", "quic", nullptr};
  const char *proto     = nullptr;
  xFlagAddChoice(set, "proto", 'p', "PROTO", "", choices, &proto, "tcp", xFlagAttr_None);

  auto r = Parse(set, {"prog", "-p", "quic"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(proto, "quic");

  xFlagSetDestroy(set);
}

TEST(Flag, ParseChoiceInvalidReportsOptions) {
  xFlagSet    set       = xFlagSetCreate("prog", nullptr);
  const char *choices[] = {"tcp", "udp", nullptr};
  const char *proto     = nullptr;
  xFlagAddChoice(set, "proto", 'p', "PROTO", "", choices, &proto, "tcp", xFlagAttr_None);

  auto r = Parse(set, {"prog", "-p", "sctp"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);
  ASSERT_NE(r.err, nullptr);
  ASSERT_NE(strstr(r.err, "tcp"), nullptr);
  ASSERT_NE(strstr(r.err, "udp"), nullptr);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseCounter) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  int      v   = 0;
  xFlagAddCounter(set, "verbose", 'v', "", &v, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-vvv"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_EQ(v, 3);

  xFlagSetDestroy(set);
}

TEST(Flag, ParseCounterSpaced) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  int      v   = 0;
  xFlagAddCounter(set, "verbose", 'v', "", &v, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-v", "-v", "--verbose"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_EQ(v, 3);

  xFlagSetDestroy(set);
}

/* ───────────────────── Required / errors ───────────────────── */

TEST(Flag, MissingRequiredFlag) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *url = nullptr;
  xFlagAddString(set, "url", 'u', "URL", "", &url, nullptr, xFlagAttr_Required);

  auto r = Parse(set, {"prog"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);
  ASSERT_NE(r.err, nullptr);
  ASSERT_NE(strstr(r.err, "url"), nullptr);

  xFlagSetDestroy(set);
}

TEST(Flag, UnknownLongFlag) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);

  auto r = Parse(set, {"prog", "--no-such"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

TEST(Flag, UnknownShortFlag) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);

  auto r = Parse(set, {"prog", "-x"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

TEST(Flag, MissingValueForLong) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *u   = nullptr;
  xFlagAddString(set, "url", 'u', "URL", "", &u, nullptr, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--url"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

TEST(Flag, EqualsOnNoArgFlagRejected) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  bool     b   = false;
  xFlagAddBool(set, "verbose", 'v', "", &b, xFlagAttr_None);

  auto r = Parse(set, {"prog", "--verbose=yes"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

/* ───────────────────── Positionals / --  ───────────────────── */

TEST(Flag, SinglePositional) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *in  = nullptr;
  xFlagAddPositional(set, "INPUT", "", &in, xFlagAttr_Required);

  auto r = Parse(set, {"prog", "file.txt"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(in, "file.txt");

  xFlagSetDestroy(set);
}

TEST(Flag, MissingRequiredPositional) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *in  = nullptr;
  xFlagAddPositional(set, "INPUT", "", &in, xFlagAttr_Required);

  auto r = Parse(set, {"prog"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

TEST(Flag, PositionalTailCollectsRemaining) {
  xFlagSet     set  = xFlagSetCreate("prog", nullptr);
  bool         v    = false;
  const char  *in   = nullptr;
  const char **tail = nullptr;
  size_t       cnt  = 0;
  xFlagAddBool(set, "verbose", 'v', "", &v, xFlagAttr_None);
  xFlagAddPositional(set, "INPUT", "", &in, xFlagAttr_Required);
  xFlagAddPositionalTail(set, "EXTRA", "", &tail, &cnt, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-v", "main.c", "a.c", "b.c"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_TRUE(v);
  ASSERT_STREQ(in, "main.c");
  ASSERT_EQ(cnt, 2u);
  ASSERT_STREQ(tail[0], "a.c");
  ASSERT_STREQ(tail[1], "b.c");
  ASSERT_EQ(tail[2], nullptr);

  xFlagSetDestroy(set);
}

TEST(Flag, DoubleDashEndsOptions) {
  xFlagSet     set  = xFlagSetCreate("prog", nullptr);
  bool         v    = false;
  const char **tail = nullptr;
  size_t       cnt  = 0;
  xFlagAddBool(set, "verbose", 'v', "", &v, xFlagAttr_None);
  xFlagAddPositionalTail(set, "ARGS", "", &tail, &cnt, xFlagAttr_None);

  auto r = Parse(set, {"prog", "-v", "--", "--not-a-flag", "-x"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_TRUE(v);
  ASSERT_EQ(cnt, 2u);
  ASSERT_STREQ(tail[0], "--not-a-flag");
  ASSERT_STREQ(tail[1], "-x");

  xFlagSetDestroy(set);
}

TEST(Flag, DashIsPositional) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *in  = nullptr;
  xFlagAddPositional(set, "INPUT", "", &in, xFlagAttr_Required);

  auto r = Parse(set, {"prog", "-"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(in, "-");

  xFlagSetDestroy(set);
}

TEST(Flag, UnexpectedExtraPositional) {
  xFlagSet    set = xFlagSetCreate("prog", nullptr);
  const char *in  = nullptr;
  xFlagAddPositional(set, "INPUT", "", &in, xFlagAttr_None);

  auto r = Parse(set, {"prog", "a", "b"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

/* ───────────────────── Multi flag ───────────────────── */

TEST(Flag, MultiStringCollectsAllOccurrences) {
  xFlagSet    set  = xFlagSetCreate("prog", nullptr);
  const char *last = nullptr;
  xFlagAddString(set, "include", 'I', "DIR", "", &last, nullptr, xFlagAttr_Multi);

  /* Multi still writes last occurrence into *storage; the rest */
  /* live inside the xFlagSet (not exposed in v1 besides that).*/
  auto r = Parse(set, {"prog", "-I/a", "-I/b", "--include=/c"});
  ASSERT_EQ(r.rc, xErrno_Ok);
  ASSERT_STREQ(last, "/c");

  xFlagSetDestroy(set);
}

/* ───────────────────── Built-in help / version ───────────────────── */

TEST(Flag, HelpReturnsAgain) {
  xFlagSet set = xFlagSetCreate("prog", "summary");
  bool     v   = false;
  xFlagAddBool(set, "verbose", 'v', "be loud", &v, xFlagAttr_None);

  /* Redirect stdout to /dev/null so help output doesn't pollute*/
  fflush(stdout);
#ifdef _WIN32
  FILE *saved = tmpfile();
  freopen("NUL", "w", stdout);
#else
  FILE *saved = stdout;
  stdout      = fopen("/dev/null", "w");
#endif
  auto r = Parse(set, {"prog", "--help"});
  fclose(stdout);
#ifdef _WIN32
  freopen(tmpnam(NULL), "w", stdout);
  fclose(saved);
#else
  stdout = saved;
#endif

  ASSERT_EQ(r.rc, xErrno_Again);

  xFlagSetDestroy(set);
}

TEST(Flag, VersionReturnsAgainWhenSet) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);
  xFlagSetVersion(set, "1.2.3");

  fflush(stdout);
#ifdef _WIN32
  FILE *saved2 = tmpfile();
  freopen("NUL", "w", stdout);
#else
  FILE *saved2 = stdout;
  stdout       = fopen("/dev/null", "w");
#endif
  auto r2 = Parse(set, {"prog", "--version"});
  fclose(stdout);
#ifdef _WIN32
  freopen(tmpnam(NULL), "w", stdout);
  fclose(saved2);
#else
  stdout = saved2;
#endif

  ASSERT_EQ(r2.rc, xErrno_Again);

  xFlagSetDestroy(set);
}

TEST(Flag, VersionNotRecognisedWhenUnset) {
  xFlagSet set = xFlagSetCreate("prog", nullptr);

  auto r = Parse(set, {"prog", "--version"});
  ASSERT_EQ(r.rc, xErrno_InvalidArg);

  xFlagSetDestroy(set);
}

/* ───────────────────── Print helpers don't crash ───────────────────── */

TEST(Flag, PrintUsageAndHelp) {
  xFlagSet    set       = xFlagSetCreate("prog", "a test");
  bool        v         = false;
  const char *url       = nullptr;
  const char *in        = nullptr;
  int         n         = 0;
  const char *choices[] = {"tcp", "udp", nullptr};
  const char *proto     = nullptr;
  xFlagAddBool(set, "verbose", 'v', "be loud", &v, xFlagAttr_None);
  xFlagAddString(set, "url", 'u', "URL", "endpoint", &url, "ws://x", xFlagAttr_Required);
  xFlagAddInt(set, "count", 'n', "N", "number", &n, 3, xFlagAttr_None);
  xFlagAddChoice(set, "proto", 'p', "PROTO", "transport", choices, &proto, "tcp", xFlagAttr_None);
  xFlagAddPositional(set, "INPUT", "input file", &in, xFlagAttr_Required);
  xFlagSetEpilog(set, "see also: moo(1)");
  xFlagSetVersion(set, "0.1.0");

  FILE *fp = fopen("/dev/null", "w");
  ASSERT_NE(fp, nullptr);
  xFlagPrintUsage(set, fp);
  xFlagPrintHelp(set, fp);
  fclose(fp);

  xFlagSetDestroy(set);
}
