/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_crypto_mbedtls.c - SHA-1 / Base64 via mbedTLS
 */

#ifdef XK_HAS_MBEDTLS

#include "ws_crypto.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif
#include <mbedtls/base64.h>

/*
 * mbedTLS 4.x removed the standalone sha1.h header.
 * Use the generic Message Digest API (md.h) which works across all versions.
 */
#if __has_include(<mbedtls/sha1.h>)
#include <mbedtls/sha1.h>
#define XKIT_MBEDTLS_HAS_SHA1_H 1
#else
#include <mbedtls/md.h>
#define XKIT_MBEDTLS_HAS_SHA1_H 0
#endif

void xWsSHA1(const unsigned char *input, size_t len,
             unsigned char *output) {
#if !XKIT_MBEDTLS_HAS_SHA1_H
  /* mbedTLS 4.x: use the generic MD API */
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
             input, len, output);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  /* mbedTLS 3.x */
  mbedtls_sha1(input, len, output);
#else
  /* mbedTLS 2.x */
  mbedtls_sha1_ret(input, len, output);
#endif
}

int xWsBase64Encode(const unsigned char *input, size_t in_len,
                    char *output, size_t out_len) {
  if (in_len == 0) {
    if (out_len > 0) output[0] = '\0';
    return 0;
  }
  size_t olen = 0;
  int ret = mbedtls_base64_encode(
    (unsigned char *)output, out_len, &olen, input, in_len);
  if (ret != 0) return -1;
  return (int)olen;
}

#endif /* XK_HAS_MBEDTLS */
