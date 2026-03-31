/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.c - Error handling implementation
 */

#include <xbase/error.h>

static const char *xErrnoStrings[] = {
  [xErrno_Ok]           = "ok",
  [xErrno_Unknown]      = "unknown error",
  [xErrno_InvalidArg]   = "invalid argument",
  [xErrno_NoMemory]     = "out of memory",
  [xErrno_InvalidState] = "invalid state",
  [xErrno_SysError]     = "system error",
  [xErrno_NotFound]     = "not found",
  [xErrno_AlreadyExists]= "already exists",
  [xErrno_Cancelled]    = "cancelled",
};

const char *xstrerror(xErrno err) {
  if (err < 0 || err >= (int)(sizeof(xErrnoStrings) / sizeof(xErrnoStrings[0]))) {
    return "unknown error";
  }
  return xErrnoStrings[err];
}
