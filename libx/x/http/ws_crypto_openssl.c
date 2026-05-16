/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_crypto_openssl.c - SHA-1 / Base64 via OpenSSL
 */

#ifdef X_HAS_OPENSSL

#include "ws_crypto.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string.h>

void xWsSHA1(const unsigned char *input, size_t len,
             unsigned char *output) {
  SHA1(input, len, output);
}

int xWsBase64Encode(const unsigned char *input, size_t in_len,
                    char *output, size_t out_len) {
  /* EVP_EncodeBlock writes ((in_len+2)/3)*4 bytes + NUL */
  size_t needed = ((in_len + 2) / 3) * 4 + 1;
  if (out_len < needed) return -1;

  int n = EVP_EncodeBlock((unsigned char *)output, input,
                          (int)in_len);
  output[n] = '\0';
  return n;
}

#endif /* X_HAS_OPENSSL */
