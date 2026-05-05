/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * url.h - Lightweight URL parser
 *
 * Parses a URL string into its components. The parser makes an internal
 * copy of the input string; all pointer fields reference this copy, so
 * the caller is free to discard the original after xUrlParse() returns.
 * Call xUrlFree() to release the internal copy when done.
 */

#ifndef XNET_URL_H
#define XNET_URL_H

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdint.h>

/**
 * @brief Parsed URL components.
 *
 * Each field is a pointer + length pair referencing an internal copy
 * of the original URL string. Fields that are absent have ptr=NULL,
 * len=0. Call xUrlFree() to release the internal copy.
 *
 * Supported format:
 *   scheme://[userinfo@]host[:port][/path][?query][#fragment]
 */
XDEF_STRUCT(xUrl) {
  char       *raw_;          /**< Owned copy (internal, do not touch) */
  const char *scheme;       /**< e.g. "http", "wss"          */
  size_t      scheme_len;

  const char *userinfo;     /**< e.g. "user:pass" (optional) */
  size_t      userinfo_len;

  const char *host;         /**< e.g. "example.com"          */
  size_t      host_len;

  const char *port;         /**< e.g. "8080" (optional)      */
  size_t      port_len;

  const char *path;         /**< e.g. "/ws/chat"             */
  size_t      path_len;

  const char *query;        /**< e.g. "key=val" (optional)   */
  size_t      query_len;

  const char *fragment;     /**< e.g. "section1" (optional)  */
  size_t      fragment_len;
};

/**
 * @brief Parse a URL string into its components.
 *
 * Makes an internal copy of @p raw. All output fields point into
 * this copy, so the caller may free or overwrite @p raw afterwards.
 * At minimum, scheme and host must be present.
 *
 * On success the caller must eventually call xUrlFree() to release
 * the internal copy. On failure the structure is zeroed and no
 * cleanup is needed.
 *
 * @param raw  NUL-terminated URL string (must not be NULL).
 * @param url  Output structure (must not be NULL).
 * @return     xErrno_Ok on success, xErrno_InvalidArg on bad input.
 */
XCAPI(xErrno) xUrlParse(const char *raw, xUrl *url);

/**
 * @brief Free the internal copy held by a parsed URL.
 *
 * After this call every pointer field in @p url is NULL.
 * Passing NULL or a zeroed xUrl is safe (no-op).
 *
 * @param url  Parsed URL to release, or NULL.
 */
XCAPI(void) xUrlFree(xUrl *url);

/**
 * @brief Return the numeric port from a parsed URL.
 *
 * If the URL contains an explicit port string, it is converted to an
 * integer. Otherwise a default port is returned based on the scheme:
 *   http / ws   → 80
 *   https / wss → 443
 *
 * @param url  Parsed URL (must not be NULL).
 * @return     Port number, or 0 if the scheme is unknown and no port
 *             was specified.
 */
XCAPI(uint16_t) xUrlPort(const xUrl *url);

#endif /* XNET_URL_H */
