/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - Error handling
 */

#ifndef XBASE_ERROR_H
#define XBASE_ERROR_H

#include <x/base/base.h>

XDEF_ENUM(xErrno){
  xErrno_Ok = 0,        /**< Success                                    */
  xErrno_Unknown,       /**< Unspecified error (legacy / catch-all)     */
  xErrno_InvalidArg,    /**< NULL or invalid argument                   */
  xErrno_NoMemory,      /**< Memory allocation failed                   */
  xErrno_InvalidState,  /**< Object is in the wrong state for this call */
  xErrno_SysError,      /**< Underlying syscall / OS error              */
  xErrno_NotFound,      /**< Requested item does not exist              */
  xErrno_AlreadyExists, /**< Item already registered / bound            */
  xErrno_Cancelled,     /**< Operation was cancelled                    */
  xErrno_NotSupported,  /**< Feature not available / not compiled in    */
  xErrno_DnsNotFound,   /**< DNS: hostname does not exist               */
  xErrno_DnsTempFail,   /**< DNS: temporary failure, try again later    */
  xErrno_DnsError,      /**< DNS: unrecoverable resolution error        */
  xErrno_Timeout,       /**< Operation timed out                        */
  xErrno_Again,         /**< Operation would block, try again later     */
  xErrno_Busy,          /**< Object is already handling a prior request */
  xErrno_Pending,       /**< Operation submitted asynchronously; completion
                             will be signalled via callback later            */
  xErrno_PromptTooLong, /**< Estimated prompt exceeds configured budget */
};

/**
 * @brief Return a human-readable error message.
 * @param err error code
 * @return error message string (never NULL)
 */
const char *xstrerror(xErrno err);

#endif // XBASE_ERROR_H
