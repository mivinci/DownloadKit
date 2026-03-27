/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - Error handling
 */

#ifndef XBASE_ERROR_H
#define XBASE_ERROR_H

#include <xbase/base.h>

XDEF_ENUM(xResult){
  xResult_Ok  = 0,
  xResult_Err = -1,
};

XDEF_ENUM(xErrno){
  xErrno_Unknown = 0,
};

/**
 * @brief Throw an error.
 * @param err error code
 * @param fmt format string.
 * @param ... arguments to the format string.
 */
void xThrow(xErrno err, const char *fmt, ...);

#endif // XBASE_ERROR_H
