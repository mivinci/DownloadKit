/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_test.cpp - Unit tests for xAgentMemory (generic + JSONL).
 *
 * The tests exercise the abstract dispatch layer (NULL-handle
 * tolerance, release idempotency, optional slot fallback) and the
 * built-in JSONL backend end-to-end: write → read round-trip for
 * each xAgentSessionEntryKind, retrieval windowing by max_entries,
 * tolerance of malformed lines, and directory auto-creation.
 *
 * Each test names its own subdirectory under a temp root so the
 * fixture can be run in parallel without file-level races.
 */

#include <gtest/gtest.h>

#include <x/agent/memory.h>
#include <x/agent/message.h>
#include <x/agent/session.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <string>

/* Minimal portable helpers so we don't depend on std::filesystem
 * (the project builds without a C++17 standard flag, and the test
 * only needs `mkdir -p` + `rm -rf` style operations on a temp
 * directory). */

static int PathExists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static void MkdirsP(const std::string &path) {
  /* Create every intermediate component; ignore EEXIST. */
  std::string cur;
  for (size_t i = 0; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty()) {
        mkdir(cur.c_str(), 0755);
      }
    }
    if (i < path.size()) cur.push_back(path[i]);
  }
}

static void RmRf(const std::string &path) {
  /* Test-only: shell out to `rm -rf`. Not used in production. */
  std::string cmd = "rm -rf '" + path + "'";
  (void)std::system(cmd.c_str());
}

/* Helper: allocate a temp directory unique to this test instance. */
static std::string TempRoot(const std::string &name) {
  const char *tmp = std::getenv("TMPDIR");
  std::string root =
    std::string(tmp && *tmp ? tmp : "/tmp") + "/xagent_memory_test_" + name;
  /* Trim any trailing slash that TMPDIR might carry. */
  if (!root.empty() && root[root.size() - 1] == '/')
    root.erase(root.size() - 1);
  /* Clean up any previous run so each invocation starts fresh. */
  RmRf(root);
  MkdirsP(root);
  return root;
}

/* Helper: fill an xAgentSessionMsg Text entry with the given role and
 * payload. Strings are borrowed from the caller; the helper does
 * not allocate. */
static xAgentSessionMsg MakeText(xAgentRole role, const char *text) {
  xAgentSessionMsg m{};
  m.role     = role;
  m.kind     = xAgentSessionEntryKind_Text;
  m.text     = text;
  m.text_len = std::strlen(text);
  return m;
}

/* ── Generic dispatch ─────────────────────────────────────────────── */

TEST(xAgentMemory, NullStoreIsNoOp) {
  xAgentMemoryQuery q{};
  q.session_id = "s";

  EXPECT_EQ(xAgentMemoryAppend(nullptr, &q,
                               xAgentMemoryAppendReason_Explicit, nullptr, 0),
            xErrno_Ok);

  xAgentMemoryHits hits{};
  EXPECT_EQ(xAgentMemoryRetrieve(nullptr, &q, &hits), xErrno_Ok);
  EXPECT_EQ(hits.n_entries, size_t{0});
  EXPECT_EQ(hits.entries, nullptr);

  /* Release on an already-zero hit set is safe. */
  xAgentMemoryReleaseHits(nullptr, &hits);

  EXPECT_EQ(xAgentMemoryOpenSession(nullptr, "s"), xErrno_Ok);
  EXPECT_EQ(xAgentMemoryCloseSession(nullptr, "s"), xErrno_Ok);
  xAgentMemoryDestroy(nullptr); /* must not crash */
}

TEST(xAgentMemory, NullOutHitsIsInvalidArg) {
  xAgentMemoryQuery q{};
  q.session_id = "s";
  EXPECT_EQ(xAgentMemoryRetrieve(nullptr, &q, nullptr), xErrno_InvalidArg);
}

/* ── JSONL factory ────────────────────────────────────────────────── */

TEST(xAgentMemoryJsonl, FactoryRejectsBadConf) {
  EXPECT_EQ(xAgentMemoryJsonlCreate(nullptr), nullptr);

  xAgentMemoryJsonlConf c{};
  EXPECT_EQ(xAgentMemoryJsonlCreate(&c), nullptr); /* root_dir NULL */

  c.root_dir = "";
  EXPECT_EQ(xAgentMemoryJsonlCreate(&c), nullptr); /* root_dir empty */
}

TEST(xAgentMemoryJsonl, CreateDestroy) {
  const std::string root = TempRoot("create_destroy");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);
  xAgentMemoryDestroy(store);
}

/* ── Append + Retrieve round trips ────────────────────────────────── */

TEST(xAgentMemoryJsonl, AppendThenRetrieveText) {
  const std::string root = TempRoot("append_retrieve_text");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "sess1";

  xAgentSessionMsg msgs[] = {
    MakeText(xAgentRole_User, "hello, world"),
    MakeText(xAgentRole_Assistant, "hi there"),
  };
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               msgs, 2),
            xErrno_Ok);

  /* The file should now exist on disk. */
  std::string path = root + "/sessions/sess1/history.jsonl";
  ASSERT_TRUE(PathExists(path));

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{2});

  EXPECT_EQ(hits.entries[0].role, xAgentRole_User);
  EXPECT_EQ(hits.entries[0].kind, xAgentSessionEntryKind_Text);
  ASSERT_NE(hits.entries[0].text, nullptr);
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("hello, world"));

  EXPECT_EQ(hits.entries[1].role, xAgentRole_Assistant);
  EXPECT_EQ(std::string(hits.entries[1].text, hits.entries[1].text_len),
            std::string("hi there"));

  xAgentMemoryReleaseHits(store, &hits);
  EXPECT_EQ(hits.entries, nullptr);
  EXPECT_EQ(hits.n_entries, size_t{0});

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, EscapesSpecialCharactersRoundTrip) {
  const std::string root = TempRoot("escape_round_trip");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  const char *tricky = "line1\nline2\twith\"quotes\"and\\backslashes";
  xAgentSessionMsg m = MakeText(xAgentRole_User, tricky);
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &m, 1),
            xErrno_Ok);

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string(tricky));
  xAgentMemoryReleaseHits(store, &hits);

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, ToolUseAndToolResultRoundTrip) {
  const std::string root = TempRoot("tool_round_trip");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  xAgentSessionMsg tool_use{};
  tool_use.role          = xAgentRole_Assistant;
  tool_use.kind          = xAgentSessionEntryKind_ToolUse;
  tool_use.tool_use_id   = "call_1";
  tool_use.tool_use_name = "shell";
  tool_use.tool_use_args = "{\"cmd\":\"ls\"}";

  xAgentSessionMsg tool_result{};
  tool_result.role                  = xAgentRole_Tool;
  tool_result.kind                  = xAgentSessionEntryKind_ToolResult;
  tool_result.tool_result_id        = "call_1";
  tool_result.tool_result_output    = "file1\nfile2";
  tool_result.tool_result_output_len = std::strlen("file1\nfile2");
  tool_result.tool_result_is_error  = 0;

  xAgentSessionMsg batch[] = {tool_use, tool_result};
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               batch, 2),
            xErrno_Ok);

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{2});

  EXPECT_EQ(hits.entries[0].kind, xAgentSessionEntryKind_ToolUse);
  ASSERT_NE(hits.entries[0].tool_use_id, nullptr);
  EXPECT_STREQ(hits.entries[0].tool_use_id, "call_1");
  EXPECT_STREQ(hits.entries[0].tool_use_name, "shell");
  ASSERT_NE(hits.entries[0].tool_use_args, nullptr);
  EXPECT_EQ(std::string(hits.entries[0].tool_use_args),
            std::string("{\"cmd\":\"ls\"}"));

  EXPECT_EQ(hits.entries[1].kind, xAgentSessionEntryKind_ToolResult);
  EXPECT_STREQ(hits.entries[1].tool_result_id, "call_1");
  EXPECT_EQ(hits.entries[1].tool_result_is_error, 0);
  EXPECT_EQ(std::string(hits.entries[1].tool_result_output,
                        hits.entries[1].tool_result_output_len),
            std::string("file1\nfile2"));

  xAgentMemoryReleaseHits(store, &hits);
  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, ToolResultIsErrorFlag) {
  const std::string root = TempRoot("tool_is_error");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  xAgentSessionMsg m{};
  m.role                   = xAgentRole_Tool;
  m.kind                   = xAgentSessionEntryKind_ToolResult;
  m.tool_result_id         = "err_1";
  m.tool_result_output     = "boom";
  m.tool_result_output_len = 4;
  m.tool_result_is_error   = 1;
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &m, 1),
            xErrno_Ok);

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(hits.entries[0].tool_result_is_error, 1);
  xAgentMemoryReleaseHits(store, &hits);
  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, MaxEntriesWindowKeepsTail) {
  const std::string root = TempRoot("max_entries_window");
  xAgentMemoryJsonlConf c{};
  c.root_dir            = root.c_str();
  c.default_max_entries = 100;
  xAgentMemory store    = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  /* Append 10 entries with increasing body text. */
  for (int i = 0; i < 10; i++) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "msg_%d", i);
    xAgentSessionMsg m = MakeText(xAgentRole_User, buf);
    ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                                 &m, 1),
              xErrno_Ok);
  }

  /* Ask for only the last 3. */
  q.max_entries = 3;
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{3});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("msg_7"));
  EXPECT_EQ(std::string(hits.entries[1].text, hits.entries[1].text_len),
            std::string("msg_8"));
  EXPECT_EQ(std::string(hits.entries[2].text, hits.entries[2].text_len),
            std::string("msg_9"));

  xAgentMemoryReleaseHits(store, &hits);
  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, PerTurnRetrievalShortCircuits) {
  /* JSONL has no semantic index, so when Session asks for a
   * per-turn retrieve (recent_turn != NULL) it MUST return an
   * empty set. The tail would otherwise duplicate what the
   * Create-time prime path already injected into history and
   * silently double-bill tokens on every user input. */
  const std::string root = TempRoot("per_turn_shortcircuit");
  xAgentMemoryJsonlConf c{};
  c.root_dir         = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  /* Seed the store with something we'd normally get back. */
  xAgentSessionMsg seed = MakeText(xAgentRole_User, "past-turn");
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &seed, 1),
            xErrno_Ok);

  /* Sanity: prime-style retrieve (recent_turn == NULL) still
   * returns the seeded entry. */
  xAgentMemoryHits prime_hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &prime_hits), xErrno_Ok);
  EXPECT_EQ(prime_hits.n_entries, size_t{1});
  xAgentMemoryReleaseHits(store, &prime_hits);

  /* Per-turn retrieve (recent_turn != NULL) must be an empty
   * no-op for this backend. */
  xAgentContent cb{};
  cb.type          = xAgentContentType_Text;
  cb.u.text.text   = "new user turn";
  cb.u.text.len    = std::strlen("new user turn");
  xAgentMessage recent{};
  recent.role     = xAgentRole_User;
  recent.contents = &cb;
  recent.n        = 1;

  q.recent_turn = &recent;
  xAgentMemoryHits turn_hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &turn_hits), xErrno_Ok);
  EXPECT_EQ(turn_hits.n_entries, size_t{0});
  EXPECT_EQ(turn_hits.cookie, nullptr);
  xAgentMemoryReleaseHits(store, &turn_hits);

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, RetrieveOnMissingFileIsEmpty) {
  const std::string root = TempRoot("missing_file");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "never_appended";

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  EXPECT_EQ(hits.n_entries, size_t{0});
  EXPECT_EQ(hits.entries, nullptr);
  xAgentMemoryReleaseHits(store, &hits);

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, MalformedLinesAreSkipped) {
  const std::string root = TempRoot("malformed_lines");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  /* Prepare a file with one good line sandwiched between garbage. */
  std::string dir = root + "/sessions/default";
  MkdirsP(dir);
  std::string path = dir + "/history.jsonl";
  {
    std::ofstream f(path.c_str());
    f << "this is not json\n";
    f << "{\"role\":\"user\",\"kind\":\"text\",\"text\":\"valid\"}\n";
    f << "{ broken json\n";
  }

  xAgentMemoryQuery q{};
  q.session_id = "default";
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("valid"));
  xAgentMemoryReleaseHits(store, &hits);

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, SeparateSessionsDoNotBleed) {
  const std::string root = TempRoot("separate_sessions");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q1{};
  q1.session_id = "s1";
  xAgentSessionMsg m1 = MakeText(xAgentRole_User, "in_s1");
  ASSERT_EQ(xAgentMemoryAppend(store, &q1, xAgentMemoryAppendReason_Explicit,
                               &m1, 1),
            xErrno_Ok);

  xAgentMemoryQuery q2{};
  q2.session_id = "s2";
  xAgentSessionMsg m2 = MakeText(xAgentRole_User, "in_s2");
  ASSERT_EQ(xAgentMemoryAppend(store, &q2, xAgentMemoryAppendReason_Explicit,
                               &m2, 1),
            xErrno_Ok);

  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q1, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("in_s1"));
  xAgentMemoryReleaseHits(store, &hits);

  ASSERT_EQ(xAgentMemoryRetrieve(store, &q2, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("in_s2"));
  xAgentMemoryReleaseHits(store, &hits);

  xAgentMemoryDestroy(store);
}

TEST(xAgentMemoryJsonl, AppendRejectsMissingIds) {
  const std::string root = TempRoot("missing_ids");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{}; /* session_id missing */
  xAgentSessionMsg m = MakeText(xAgentRole_User, "x");
  EXPECT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &m, 1),
            xErrno_InvalidArg);

  xAgentMemoryDestroy(store);
}

/* Each freshly-written JSONL line carries the entry's creation
 * timestamp in the "ts" field. The preferred source is the
 * @c created_at_ms the caller stamped when producing the entry —
 * only if that is zero does the backend fall back to the current
 * wall-clock. Future backends that want to sort by time can read
 * either shape off disk, and legacy files without "ts" keep
 * parsing fine (see ReadsLegacyLinesWithoutTs below). */
TEST(xAgentMemoryJsonl, AppendPersistsExplicitCreatedAtMs) {
  const std::string root = TempRoot("ts_field_explicit");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";
  xAgentSessionMsg m = MakeText(xAgentRole_User, "hi");
  /* A distinctive value that wall-clock can't realistically produce
   * on the same machine at the same instant, so we can prove the
   * backend used our stamp rather than a fresh read. */
  m.created_at_ms = 1700000000001ULL;
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &m, 1),
            xErrno_Ok);

  std::string path = root + "/sessions/s/history.jsonl";
  std::ifstream f(path);
  ASSERT_TRUE(f.is_open());
  std::string line;
  std::getline(f, line);
  EXPECT_NE(line.find("\"ts\":1700000000001"), std::string::npos) << line;

  xAgentMemoryDestroy(store);
}

/* When the caller hands in an entry with @c created_at_ms == 0
 * (e.g. hand-crafted input from a test that doesn't set it), the
 * backend falls back to the current wall-clock so every persisted
 * line still ends up with a value. */
TEST(xAgentMemoryJsonl, AppendFallsBackToWallClockWhenUnset) {
  const std::string root = TempRoot("ts_field_fallback");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";
  xAgentSessionMsg m = MakeText(xAgentRole_User, "hi");
  /* MakeText already leaves created_at_ms == 0; assert it for
   * the record in case MakeText ever changes. */
  ASSERT_EQ(m.created_at_ms, 0ULL);
  ASSERT_EQ(xAgentMemoryAppend(store, &q, xAgentMemoryAppendReason_Explicit,
                               &m, 1),
            xErrno_Ok);

  std::string path = root + "/sessions/s/history.jsonl";
  std::ifstream f(path);
  ASSERT_TRUE(f.is_open());
  std::string line;
  std::getline(f, line);
  ASSERT_NE(line.find("\"ts\":"), std::string::npos);

  size_t pos = line.find("\"ts\":");
  pos += std::strlen("\"ts\":");
  long long v = 0;
  while (pos < line.size() && std::isdigit((unsigned char)line[pos])) {
    v = v * 10 + (line[pos] - '0');
    pos++;
  }
  EXPECT_GT(v, 1000000000000LL); /* plausible ms-epoch lower bound */

  xAgentMemoryDestroy(store);
}

/* Legacy files (pre-timestamp) still parse: write a line by hand
 * without "ts" and make sure retrieve hands it back intact. */
TEST(xAgentMemoryJsonl, ReadsLegacyLinesWithoutTs) {
  const std::string root = TempRoot("legacy_no_ts");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  std::string dir = root + "/sessions/s";
  MkdirsP(dir);
  {
    std::ofstream f(dir + "/history.jsonl");
    /* Exactly the shape the pre-migration agent wrote. */
    f << "{\"role\":\"user\",\"kind\":\"text\",\"text\":\"legacy\"}\n";
  }

  xAgentMemoryQuery q{};
  q.session_id = "s";
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{1});
  EXPECT_EQ(hits.entries[0].role, xAgentRole_User);
  EXPECT_EQ(std::string(hits.entries[0].text, hits.entries[0].text_len),
            std::string("legacy"));
  xAgentMemoryReleaseHits(store, &hits);

  xAgentMemoryDestroy(store);
}

/* When a summary entry exists in the JSONL, Retrieve must only
 * return entries from the last summary onward — everything
 * before it has been absorbed. This test writes a file with
 * pre-summary entries, a summary, and post-summary entries, and
 * verifies that only the summary + post-summary entries come back. */
TEST(xAgentMemoryJsonl, RetrieveStartsFromLastSummary) {
  const std::string root = TempRoot("summary_start");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  /* Append pre-summary entries. */
  xAgentMemoryQuery q{};
  q.session_id = "s";
  xAgentSessionMsg pre = MakeText(xAgentRole_User, "old_message");
  ASSERT_EQ(xAgentMemoryAppend(store, &q,
                               xAgentMemoryAppendReason_Compacted, &pre, 1),
            xErrno_Ok);

  /* Append the summary entry. */
  xAgentSessionMsg summary{};
  summary.role        = xAgentRole_Summary;
  summary.kind        = xAgentSessionEntryKind_Text;
  summary.text        = "[summary] conversation so far";
  summary.text_len    = std::strlen(summary.text);
  ASSERT_EQ(xAgentMemoryAppend(store, &q,
                               xAgentMemoryAppendReason_Compacted,
                               &summary, 1),
            xErrno_Ok);

  /* Append post-summary entries. */
  xAgentSessionMsg post1 = MakeText(xAgentRole_User, "new_question");
  xAgentSessionMsg post2 = MakeText(xAgentRole_Assistant, "new_answer");
  xAgentSessionMsg post_arr[] = {post1, post2};
  ASSERT_EQ(xAgentMemoryAppend(store, &q,
                               xAgentMemoryAppendReason_Explicit,
                               post_arr, 2),
            xErrno_Ok);

  /* Retrieve: should get summary + 2 post entries = 3. */
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{3});

  /* First entry must be the summary. */
  EXPECT_EQ(hits.entries[0].role, xAgentRole_Summary);
  EXPECT_NE(std::string(hits.entries[0].text, hits.entries[0].text_len)
              .find("[summary]"),
            std::string::npos);

  /* Pre-summary entry must not appear anywhere. */
  for (size_t i = 0; i < hits.n_entries; i++) {
    if (hits.entries[i].text) {
      std::string t(hits.entries[i].text, hits.entries[i].text_len);
      EXPECT_EQ(t.find("old_message"), std::string::npos)
          << "pre-summary entry must not be loaded";
    }
  }

  xAgentMemoryReleaseHits(store, &hits);
  xAgentMemoryDestroy(store);
}

/* When the cap window would include pre-summary entries but the
 * summary is beyond the cap boundary, we still start from the
 * summary — even if it means returning fewer entries than @p cap. */
TEST(xAgentMemoryJsonl, SummaryOverridesCapBoundary) {
  const std::string root = TempRoot("summary_overrides_cap");
  xAgentMemoryJsonlConf c{};
  c.root_dir = root.c_str();
  /* Set a small default_max_entries so the cap window is tight. */
  c.default_max_entries = 4;
  xAgentMemory store = xAgentMemoryJsonlCreate(&c);
  ASSERT_NE(store, nullptr);

  xAgentMemoryQuery q{};
  q.session_id = "s";

  /* Write 5 pre-summary entries (more than the cap of 4). */
  for (int i = 0; i < 5; i++) {
    std::string text = "pre_" + std::to_string(i);
    xAgentSessionMsg m = MakeText(xAgentRole_User, text.c_str());
    ASSERT_EQ(xAgentMemoryAppend(store, &q,
                                 xAgentMemoryAppendReason_Compacted, &m, 1),
              xErrno_Ok);
  }

  /* Write the summary. */
  xAgentSessionMsg summary{};
  summary.role        = xAgentRole_Summary;
  summary.kind        = xAgentSessionEntryKind_Text;
  summary.text        = "[summary] compacted history";
  summary.text_len    = std::strlen(summary.text);
  ASSERT_EQ(xAgentMemoryAppend(store, &q,
                               xAgentMemoryAppendReason_Compacted,
                               &summary, 1),
            xErrno_Ok);

  /* Write 2 post-summary entries. */
  xAgentSessionMsg post1 = MakeText(xAgentRole_User, "after_summary");
  xAgentSessionMsg post2 = MakeText(xAgentRole_Assistant, "response");
  xAgentSessionMsg post_arr[] = {post1, post2};
  ASSERT_EQ(xAgentMemoryAppend(store, &q,
                               xAgentMemoryAppendReason_Explicit,
                               post_arr, 2),
            xErrno_Ok);

  /* With cap=4, the tail window would normally be entries 4-7
   * (the last 4 of 8). But the summary is at index 5, so
   * take_from should be 5, returning entries 5,6,7 = 3. */
  xAgentMemoryHits hits{};
  ASSERT_EQ(xAgentMemoryRetrieve(store, &q, &hits), xErrno_Ok);
  ASSERT_EQ(hits.n_entries, size_t{3});

  /* First must be summary. */
  EXPECT_EQ(hits.entries[0].role, xAgentRole_Summary);

  /* No pre-summary entries. */
  for (size_t i = 0; i < hits.n_entries; i++) {
    if (hits.entries[i].text) {
      std::string t(hits.entries[i].text, hits.entries[i].text_len);
      EXPECT_EQ(t.find("pre_"), std::string::npos);
    }
  }

  xAgentMemoryReleaseHits(store, &hits);
  xAgentMemoryDestroy(store);
}
