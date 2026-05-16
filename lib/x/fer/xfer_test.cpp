/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_test.cpp - xfer module unit tests
 */

#include <gtest/gtest.h>

extern "C" {
#include "xfer.h"
#include <x/base/event.h>
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ───────────────────── Test Fixture ───────────────────── */

class XferTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override { loop = xEventLoopCreate(); }

  void TearDown() override {
    if (loop) {
      xEventLoopDestroy(loop);
      loop = nullptr;
    }
  }

  /* Helper: create a minimal xTransferConf. */
  xTransferConf make_conf() {
    xTransferConf conf;
    memset(&conf, 0, sizeof(conf));
    return conf;
  }

  /* Helper: create a temporary file with some content. */
  static std::string create_temp_file(size_t size) {
    char path[] = "/tmp/xfer_test_XXXXXX";
    int fd = mkstemp(path);
    EXPECT_GE(fd, 0);
    if (fd < 0) return "";

    /* Write `size` bytes of dummy data */
    std::vector<uint8_t> data(size, 0xAB);
    ssize_t written = write(fd, data.data(), data.size());
    EXPECT_EQ(written, (ssize_t)size);
    close(fd);
    return std::string(path);
  }

  static void remove_temp_file(const std::string &path) {
    if (!path.empty()) std::remove(path.c_str());
  }
};

/* ───────────────────── Create / Destroy ───────────────────── */

TEST_F(XferTest, CreateDestroy) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferGetState(xfer), xTransferState_Idle);
  EXPECT_EQ(xTransferGetRole(xfer), xTransferRole_Sender); /* default */

  xTransferDestroy(xfer);
}

TEST_F(XferTest, CreateNullLoop) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(nullptr, &conf);
  EXPECT_EQ(xfer, nullptr);
}

TEST_F(XferTest, CreateNullConf) {
  xTransfer xfer = xTransferCreate(loop, nullptr);
  EXPECT_EQ(xfer, nullptr);
}

TEST_F(XferTest, DestroyNull) {
  /* Should not crash. */
  xTransferDestroy(nullptr);
}

/* ───────────────────── SendFile Parameter Validation ───────────────────── */

TEST_F(XferTest, SendFileNullXfer) {
  EXPECT_EQ(xTransferSendFile(nullptr, "/tmp/foo"), xErrno_InvalidArg);
}

TEST_F(XferTest, SendFileNullPath) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferSendFile(xfer, nullptr), xErrno_InvalidArg);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, SendFileNonExistent) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  /* File does not exist → should fail with SysError. */
  xErrno err = xTransferSendFile(xfer, "/tmp/xfer_nonexistent_file_12345");
  EXPECT_EQ(err, xErrno_SysError);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, SendFileValidFile) {
  std::string path = create_temp_file(1024);
  ASSERT_FALSE(path.empty());

  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  xErrno err = xTransferSendFile(xfer, path.c_str());
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_WaitingPeer);
  EXPECT_EQ(xTransferGetRole(xfer), xTransferRole_Sender);

  xTransferDestroy(xfer);
  remove_temp_file(path);
}

TEST_F(XferTest, SendFileDoubleCall) {
  std::string path = create_temp_file(256);
  ASSERT_FALSE(path.empty());

  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferSendFile(xfer, path.c_str()), xErrno_Ok);
  /* Second call should fail: not in Idle state. */
  EXPECT_EQ(xTransferSendFile(xfer, path.c_str()), xErrno_InvalidState);

  xTransferDestroy(xfer);
  remove_temp_file(path);
}

/* ───────────────────── RecvFile Parameter Validation ───────────────────── */

TEST_F(XferTest, RecvFileNullXfer) {
  EXPECT_EQ(xTransferRecvFile(nullptr, "abc", "/tmp"), xErrno_InvalidArg);
}

TEST_F(XferTest, RecvFileNullCode) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferRecvFile(xfer, nullptr, "/tmp"), xErrno_InvalidArg);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, RecvFileNullDestDir) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferRecvFile(xfer, "abc", nullptr), xErrno_InvalidArg);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, RecvFileValid) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  xErrno err = xTransferRecvFile(xfer, "7-guitar-piano", "/tmp");
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_WaitingPeer);
  EXPECT_EQ(xTransferGetRole(xfer), xTransferRole_Receiver);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, RecvFileDoubleCall) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferRecvFile(xfer, "abc", "/tmp"), xErrno_Ok);
  /* Second call should fail: not in Idle state. */
  EXPECT_EQ(xTransferRecvFile(xfer, "def", "/tmp"), xErrno_InvalidState);

  xTransferDestroy(xfer);
}

/* ───────────────────── Cancel ───────────────────── */

TEST_F(XferTest, CancelIdle) {
  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  /* Cancel from Idle → should move to Failed. */
  xTransferCancel(xfer);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_Failed);

  xTransferDestroy(xfer);
}

TEST_F(XferTest, CancelAfterSend) {
  std::string path = create_temp_file(512);
  ASSERT_FALSE(path.empty());

  xTransferConf conf = make_conf();
  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  EXPECT_EQ(xTransferSendFile(xfer, path.c_str()), xErrno_Ok);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_WaitingPeer);

  xTransferCancel(xfer);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_Failed);

  /* Cancel again should be a no-op (already Failed). */
  xTransferCancel(xfer);
  EXPECT_EQ(xTransferGetState(xfer), xTransferState_Failed);

  xTransferDestroy(xfer);
  remove_temp_file(path);
}

TEST_F(XferTest, CancelNull) {
  /* Should not crash. */
  xTransferCancel(nullptr);
}

/* ───────────────────── Accessors with NULL ───────────────────── */

TEST_F(XferTest, GetStateNull) {
  EXPECT_EQ(xTransferGetState(nullptr), xTransferState_Idle);
}

TEST_F(XferTest, GetRoleNull) {
  EXPECT_EQ(xTransferGetRole(nullptr), xTransferRole_Sender);
}

/* ───────────────────── Callbacks ───────────────────── */

namespace {
struct CallbackCtx {
  int state_change_count;
  xTransferState last_state;
  int error_count;
  xErrno last_error;
};

void on_state_change(xTransfer xfer, xTransferState state, void *ctx) {
  (void)xfer;
  auto *c = (CallbackCtx *)ctx;
  c->state_change_count++;
  c->last_state = state;
}

void on_error(xTransfer xfer, xErrno err, const char *msg, void *ctx) {
  (void)xfer;
  (void)msg;
  auto *c = (CallbackCtx *)ctx;
  c->error_count++;
  c->last_error = err;
}
} // namespace

TEST_F(XferTest, StateChangeCallback) {
  CallbackCtx cb_ctx{};

  xTransferConf conf = make_conf();
  conf.on_state_change = on_state_change;
  conf.ctx = &cb_ctx;

  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  std::string path = create_temp_file(1024);
  ASSERT_FALSE(path.empty());

  /* SendFile → state changes to WaitingPeer */
  xTransferSendFile(xfer, path.c_str());
  EXPECT_GE(cb_ctx.state_change_count, 1);
  EXPECT_EQ(cb_ctx.last_state, xTransferState_WaitingPeer);

  xTransferDestroy(xfer);
  remove_temp_file(path);
}

TEST_F(XferTest, ErrorCallbackOnBadFile) {
  CallbackCtx cb_ctx{};

  xTransferConf conf = make_conf();
  conf.on_state_change = on_state_change;
  conf.on_error = on_error;
  conf.ctx = &cb_ctx;

  xTransfer xfer = xTransferCreate(loop, &conf);
  ASSERT_NE(xfer, nullptr);

  xErrno err = xTransferSendFile(xfer, "/tmp/xfer_nonexistent_12345");
  EXPECT_EQ(err, xErrno_SysError);
  EXPECT_GE(cb_ctx.error_count, 1);
  EXPECT_EQ(cb_ctx.last_error, xErrno_SysError);
  EXPECT_EQ(cb_ctx.last_state, xTransferState_Failed);

  xTransferDestroy(xfer);
}
