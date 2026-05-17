/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.c - Error handling implementation
 */

#include <x/base/error.h>

static const char *xErrnoStrings[] = {
  [xErrno_Ok]            = "ok",
  [xErrno_Unknown]       = "unknown error",
  [xErrno_InvalidArg]    = "invalid argument",
  [xErrno_NoMemory]      = "out of memory",
  [xErrno_InvalidState]  = "invalid state",
  [xErrno_SysError]      = "system error",
  [xErrno_NotFound]      = "not found",
  [xErrno_AlreadyExists] = "already exists",
  [xErrno_Cancelled]     = "cancelled",
  [xErrno_NotSupported]  = "not supported",
  [xErrno_DnsNotFound]   = "dns: hostname not found",
  [xErrno_DnsTempFail]   = "dns: temporary failure",
  [xErrno_DnsError]      = "dns: resolution error",
  [xErrno_Timeout]       = "operation timed out",
  [xErrno_Again]         = "try again",
  [xErrno_Busy]          = "resource busy",
  [xErrno_Pending]       = "operation pending",
  [xErrno_PromptTooLong] = "prompt exceeds context budget",
};

const char *xstrerror(xErrno err) {
  if (err < 0 || err >= (int)(sizeof(xErrnoStrings) / sizeof(xErrnoStrings[0]))) {
    return "unknown error";
  }
  return xErrnoStrings[err];
}
