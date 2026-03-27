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

XDEF_ENUM(xErrno){
  xErrno_Ok             = 0,
  xErrno_Unknown
};

/**
 * @brief Return a human-readable error message.
 * @param err error code
 * @return error message string (never NULL)
 */
const char *xstrerror(xErrno err);

#endif // XBASE_ERROR_H
