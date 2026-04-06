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
#include <mbedtls/sha1.h>

void xWsSHA1(const unsigned char *input, size_t len,
             unsigned char *output) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  /* mbedTLS 3.x+: mbedtls_sha1() was removed, use _ret variant
   * or the one-shot API. mbedtls_sha1_ret was renamed to
   * mbedtls_sha1 in 3.x, but the signature takes a return. */
  mbedtls_sha1(input, len, output);
#else
  /* mbedTLS 2.x */
  mbedtls_sha1_ret(input, len, output);
#endif
}

int xWsBase64Encode(const unsigned char *input, size_t in_len,
                    char *output, size_t out_len) {
  size_t olen = 0;
  int ret = mbedtls_base64_encode(
    (unsigned char *)output, out_len, &olen, input, in_len);
  if (ret != 0) return -1;
  return (int)olen;
}

#endif /* XK_HAS_MBEDTLS */
