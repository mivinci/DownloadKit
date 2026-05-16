/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_vfs.h - Virtual File System interface for xfer
 *
 * Abstracts all data I/O (reading source file, writing received file)
 * so that callers can plug in custom storage backends (e.g. in-memory,
 * encrypted, cloud-backed).  When no custom VFS is provided, the
 * built-in POSIX implementation (fopen/fread/fwrite) is used.
 */

#ifndef XFER_VFS_H
#define XFER_VFS_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief VFS interface for custom storage backends.
 *
 * All data I/O (reading source file, writing received file) goes through
 * this interface.  When NULL is passed in xTransferConf, a default POSIX
 * implementation (fopen/fread/fwrite) is used.
 *
 * The @c ctx pointer stored in the struct is forwarded as the first
 * argument to every callback so that implementations can carry state.
 */
XDEF_STRUCT(xTransferVfs) {
  void *ctx; /**< Opaque context forwarded to all callbacks. */

  /**
   * Open a file.  Returns an opaque handle, or NULL on failure.
   * @p mode follows the same convention as fopen ("rb", "wb", "r+b").
   */
  void *(*open)(void *ctx, const char *path, const char *mode);

  /** Random-access read at @p offset.  Writes actual bytes read to @p nread. */
  xErrno (*pread)(void *ctx, void *handle, uint8_t *buf, size_t len,
                  uint64_t offset, size_t *nread);

  /** Random-access write at @p offset.  Writes actual bytes written to @p nwritten. */
  xErrno (*pwrite)(void *ctx, void *handle, const uint8_t *buf, size_t len,
                   uint64_t offset, size_t *nwritten);

  /** Get total size of the opened file. */
  xErrno (*size)(void *ctx, void *handle, uint64_t *out_size);

  /** Pre-allocate / truncate storage.  May be NULL (optional). */
  xErrno (*truncate)(void *ctx, void *handle, uint64_t size);

  /** Flush buffered data to persistent storage. */
  xErrno (*flush)(void *ctx, void *handle);

  /** Close the handle. */
  void (*close)(void *ctx, void *handle);

  /** Rename a file.  May be NULL (optional). */
  xErrno (*rename)(void *ctx, const char *from, const char *to);

  /** Remove a file.  May be NULL (optional). */
  xErrno (*remove)(void *ctx, const char *path);
};

/**
 * @brief Return the built-in POSIX VFS (fopen / fread / fwrite).
 *
 * The returned pointer is valid for the lifetime of the process.
 */
XCAPI(const xTransferVfs *) xTransferPosixVfs(void);

#endif /* XFER_VFS_H */
