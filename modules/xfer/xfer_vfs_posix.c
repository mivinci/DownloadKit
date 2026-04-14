/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_vfs_posix.c - Default POSIX VFS implementation for xfer
 *
 * Implements the xTransferVfs interface using standard C stdio
 * (fopen / fread / fwrite / fseek / ftell / fclose).
 */

#include "xfer_vfs.h"

#include <stdio.h>

/* ───────────────────── POSIX VFS callbacks ───────────────────── */

static void *posix_open(void *ctx, const char *path, const char *mode) {
  (void)ctx;
  return (void *)fopen(path, mode);
}

static xErrno posix_pread(void *ctx, void *handle, uint8_t *buf, size_t len,
                          uint64_t offset, size_t *nread) {
  (void)ctx;
  FILE *fp = (FILE *)handle;
  if (fseek(fp, (long)offset, SEEK_SET) != 0) return xErrno_SysError;
  *nread = fread(buf, 1, len, fp);
  return ferror(fp) ? xErrno_SysError : xErrno_Ok;
}

static xErrno posix_pwrite(void *ctx, void *handle, const uint8_t *buf,
                           size_t len, uint64_t offset, size_t *nwritten) {
  (void)ctx;
  FILE *fp = (FILE *)handle;
  if (fseek(fp, (long)offset, SEEK_SET) != 0) return xErrno_SysError;
  *nwritten = fwrite(buf, 1, len, fp);
  return (*nwritten == len) ? xErrno_Ok : xErrno_SysError;
}

static xErrno posix_size(void *ctx, void *handle, uint64_t *out_size) {
  (void)ctx;
  FILE *fp = (FILE *)handle;
  long cur = ftell(fp);
  if (cur < 0) return xErrno_SysError;
  if (fseek(fp, 0, SEEK_END) != 0) return xErrno_SysError;
  long end = ftell(fp);
  if (end < 0) { fseek(fp, cur, SEEK_SET); return xErrno_SysError; }
  fseek(fp, cur, SEEK_SET);
  *out_size = (uint64_t)end;
  return xErrno_Ok;
}

static xErrno posix_truncate(void *ctx, void *handle, uint64_t size) {
  (void)ctx;
  FILE *fp = (FILE *)handle;
  if (size > 0) {
    fseek(fp, (long)(size - 1), SEEK_SET);
    fputc(0, fp);
    fflush(fp);
  }
  return xErrno_Ok;
}

static xErrno posix_flush(void *ctx, void *handle) {
  (void)ctx;
  return fflush((FILE *)handle) == 0 ? xErrno_Ok : xErrno_SysError;
}

static void posix_close(void *ctx, void *handle) {
  (void)ctx;
  if (handle) fclose((FILE *)handle);
}

static xErrno posix_rename(void *ctx, const char *from, const char *to) {
  (void)ctx;
  return rename(from, to) == 0 ? xErrno_Ok : xErrno_SysError;
}

static xErrno posix_remove(void *ctx, const char *path) {
  (void)ctx;
  return remove(path) == 0 ? xErrno_Ok : xErrno_SysError;
}

/* ───────────────────── Singleton instance ───────────────────── */

static const xTransferVfs g_posix_vfs = {
  .ctx      = NULL,
  .open     = posix_open,
  .pread    = posix_pread,
  .pwrite   = posix_pwrite,
  .size     = posix_size,
  .truncate = posix_truncate,
  .flush    = posix_flush,
  .close    = posix_close,
  .rename   = posix_rename,
  .remove   = posix_remove,
};

const xTransferVfs *xTransferPosixVfs(void) { return &g_posix_vfs; }
