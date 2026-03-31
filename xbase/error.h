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
  xErrno_Ok             = 0,  /**< Success                                    */
  xErrno_Unknown,             /**< Unspecified error (legacy / catch-all)     */
  xErrno_InvalidArg,          /**< NULL or invalid argument                   */
  xErrno_NoMemory,            /**< Memory allocation failed                   */
  xErrno_InvalidState,        /**< Object is in the wrong state for this call */
  xErrno_SysError,            /**< Underlying syscall / OS error              */
  xErrno_NotFound,            /**< Requested item does not exist              */
  xErrno_AlreadyExists,       /**< Item already registered / bound            */
  xErrno_Cancelled,           /**< Operation was cancelled                    */
};

/**
 * @brief Return a human-readable error message.
 * @param err error code
 * @return error message string (never NULL)
 */
const char *xstrerror(xErrno err);

#endif // XBASE_ERROR_H
