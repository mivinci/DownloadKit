/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_attr.c - STUN attribute encoding / decoding (RFC 5389)
 */

#include "stun_attr.h"

#include <x/crypto/crc32.h>
#include <x/crypto/hmac_sha1.h>

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Writer Helpers ───────────────────── */

void xStunAttrWriterInit(xStunAttrWriter *w, uint8_t *buf, size_t cap) {
  w->buf = buf;
  w->cap = cap;
  w->pos = 0;
}

/**
 * Write a raw TLV attribute header + value, with 4-byte padding.
 * Returns xErrno_Ok on success.
 */
static xErrno attr_write_raw(xStunAttrWriter *w, uint16_t type,
                             const uint8_t *value, uint16_t value_len) {
  size_t padded = XSTUN_ALIGN4(value_len);
  size_t total  = XSTUN_ATTR_HEADER_SIZE + padded;
  if (w->pos + total > w->cap) return xErrno_NoMemory;

  uint8_t *p = w->buf + w->pos;
  xWriteU16BE(p, type);
  xWriteU16BE(p + 2, value_len);
  if (value_len > 0 && value) {
    memcpy(p + XSTUN_ATTR_HEADER_SIZE, value, value_len);
  }
  /* Zero-fill padding */
  if (padded > value_len) {
    memset(p + XSTUN_ATTR_HEADER_SIZE + value_len, 0, padded - value_len);
  }
  w->pos += total;
  return xErrno_Ok;
}

/* ───────────────────── Address Encoding ───────────────────── */

static xErrno encode_address(xStunAttrWriter *w, uint16_t attr_type,
                             const struct sockaddr *addr, bool xor_encode,
                             const uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  uint8_t  value[20]; /* max: 1 + 1 + 2 + 16 (IPv6) */
  uint16_t value_len;

  value[0] = 0; /* reserved */

  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
    value[1]                     = 0x01; /* IPv4 family */
    uint16_t port                = ntohs(a4->sin_port);
    uint32_t ip                  = ntohl(a4->sin_addr.s_addr);

    if (xor_encode) {
      port ^= (uint16_t)(XSTUN_MAGIC_COOKIE >> 16);
      ip ^= XSTUN_MAGIC_COOKIE;
    }

    xWriteU16BE(value + 2, port);
    xWriteU32BE(value + 4, ip);
    value_len = 8;
  } else if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
    value[1]                      = 0x02; /* IPv6 family */
    uint16_t port                 = ntohs(a6->sin6_port);

    if (xor_encode) {
      port ^= (uint16_t)(XSTUN_MAGIC_COOKIE >> 16);
    }
    xWriteU16BE(value + 2, port);

    /* Copy address bytes */
    memcpy(value + 4, a6->sin6_addr.s6_addr, 16);

    if (xor_encode) {
      /* XOR with magic cookie (4 bytes) + txn_id (12 bytes) */
      uint8_t xor_key[16];
      xWriteU32BE(xor_key, XSTUN_MAGIC_COOKIE);
      memcpy(xor_key + 4, txn_id, XSTUN_TXN_ID_SIZE);
      for (int i = 0; i < 16; i++) {
        value[4 + i] ^= xor_key[i];
      }
    }
    value_len = 20;
  } else {
    return xErrno_InvalidArg;
  }

  return attr_write_raw(w, attr_type, value, value_len);
}

xErrno xStunAttrWriteXorMappedAddress(xStunAttrWriter       *w,
                                      const struct sockaddr *addr,
                                      const uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  return encode_address(w, xStunAttrType_XorMappedAddress, addr, true, txn_id);
}

xErrno xStunAttrWriteMappedAddress(xStunAttrWriter       *w,
                                   const struct sockaddr *addr) {
  return encode_address(w, xStunAttrType_MappedAddress, addr, false, NULL);
}

xErrno xStunAttrWriteXorPeerAddress(xStunAttrWriter       *w,
                                    const struct sockaddr *addr,
                                    const uint8_t txn_id[XSTUN_TXN_ID_SIZE]) {
  return encode_address(w, xStunAttrType_XorPeerAddress, addr, true, txn_id);
}

/* ───────────────────── String Attributes ───────────────────── */

xErrno xStunAttrWriteUsername(xStunAttrWriter *w, const char *remote_ufrag,
                              const char *local_ufrag) {
  if (!remote_ufrag || !local_ufrag) return xErrno_InvalidArg;

  size_t rlen  = strlen(remote_ufrag);
  size_t llen  = strlen(local_ufrag);
  size_t total = rlen + 1 + llen; /* "remote:local" */

  uint8_t value[256];
  if (total > sizeof(value)) return xErrno_NoMemory;

  memcpy(value, remote_ufrag, rlen);
  value[rlen] = ':';
  memcpy(value + rlen + 1, local_ufrag, llen);

  return attr_write_raw(w, xStunAttrType_Username, value, (uint16_t)total);
}

xErrno xStunAttrWriteRealm(xStunAttrWriter *w, const char *realm) {
  if (!realm) return xErrno_InvalidArg;
  size_t len = strlen(realm);
  return attr_write_raw(w, xStunAttrType_Realm, (const uint8_t *)realm,
                        (uint16_t)len);
}

xErrno xStunAttrWriteNonce(xStunAttrWriter *w, const char *nonce) {
  if (!nonce) return xErrno_InvalidArg;
  size_t len = strlen(nonce);
  return attr_write_raw(w, xStunAttrType_Nonce, (const uint8_t *)nonce,
                        (uint16_t)len);
}

xErrno xStunAttrWriteSoftware(xStunAttrWriter *w, const char *software) {
  if (!software) return xErrno_InvalidArg;
  size_t len = strlen(software);
  return attr_write_raw(w, xStunAttrType_Software, (const uint8_t *)software,
                        (uint16_t)len);
}

/* ───────────────────── Integrity / Fingerprint ───────────────────── */

xErrno xStunAttrWriteMessageIntegrity(xStunAttrWriter *w, uint8_t *msg_buf,
                                      const uint8_t *key, size_t key_len) {
  if (!w || !msg_buf || !key) return xErrno_InvalidArg;

  /*
   * Per RFC 5389 §15.4: the length field in the STUN header is
   * temporarily adjusted to include the MESSAGE-INTEGRITY attribute
   * (but not any attributes after it).
   */
  size_t mi_offset = w->pos; /* offset within attribute area */

  /* Temporarily set the STUN header length field */
  uint16_t orig_len = xReadU16BE(msg_buf + 2);
  xWriteU16BE(msg_buf + 2, (uint16_t)(mi_offset + XSTUN_ATTR_HEADER_SIZE +
                                      XSTUN_SHA1_DIGEST_SIZE));

  /* Compute HMAC-SHA1 over header + attrs so far */
  uint8_t hmac[XSTUN_SHA1_DIGEST_SIZE];
  xHmacSha1(key, key_len, msg_buf, XSTUN_HEADER_SIZE + mi_offset, hmac);

  /* Restore original length (will be updated by caller) */
  xWriteU16BE(msg_buf + 2, orig_len);

  return attr_write_raw(w, xStunAttrType_MessageIntegrity, hmac,
                        XSTUN_SHA1_DIGEST_SIZE);
}

xErrno xStunAttrWriteFingerprint(xStunAttrWriter *w, uint8_t *msg_buf) {
  if (!w || !msg_buf) return xErrno_InvalidArg;

  size_t fp_offset = w->pos;
  /* Temporarily set length to include FINGERPRINT */
  uint16_t orig_len = xReadU16BE(msg_buf + 2);
  xWriteU16BE(msg_buf + 2, (uint16_t)(fp_offset + XSTUN_ATTR_HEADER_SIZE + 4));

  /* CRC-32 over header + attrs so far */
  uint32_t crc = xCrc32(msg_buf, XSTUN_HEADER_SIZE + fp_offset);
  crc ^= XSTUN_FINGERPRINT_XOR;

  xWriteU16BE(msg_buf + 2, orig_len);

  uint8_t value[4];
  xWriteU32BE(value, crc);
  return attr_write_raw(w, xStunAttrType_Fingerprint, value, 4);
}

/* ───────────────────── ICE Attributes ───────────────────── */

xErrno xStunAttrWritePriority(xStunAttrWriter *w, uint32_t priority) {
  uint8_t value[4];
  xWriteU32BE(value, priority);
  return attr_write_raw(w, xStunAttrType_Priority, value, 4);
}

xErrno xStunAttrWriteUseCandidate(xStunAttrWriter *w) {
  return attr_write_raw(w, xStunAttrType_UseCandidate, NULL, 0);
}

xErrno xStunAttrWriteIceControlling(xStunAttrWriter *w, uint64_t tie_breaker) {
  uint8_t value[8];
  xWriteU64BE(value, tie_breaker);
  return attr_write_raw(w, xStunAttrType_IceControlling, value, 8);
}

xErrno xStunAttrWriteIceControlled(xStunAttrWriter *w, uint64_t tie_breaker) {
  uint8_t value[8];
  xWriteU64BE(value, tie_breaker);
  return attr_write_raw(w, xStunAttrType_IceControlled, value, 8);
}

/* ───────────────────── TURN Attributes ───────────────────── */

xErrno xStunAttrWriteLifetime(xStunAttrWriter *w, uint32_t lifetime) {
  uint8_t value[4];
  xWriteU32BE(value, lifetime);
  return attr_write_raw(w, xStunAttrType_Lifetime, value, 4);
}

xErrno xStunAttrWriteRequestedTransport(xStunAttrWriter *w,
                                        uint32_t         transport) {
  uint8_t value[4];
  xWriteU32BE(value, transport);
  return attr_write_raw(w, xStunAttrType_RequestedTransport, value, 4);
}

xErrno xStunAttrWriteChannelNumber(xStunAttrWriter *w, uint16_t channel) {
  uint8_t value[4];
  xWriteU16BE(value, channel);
  xWriteU16BE(value + 2, 0); /* RFFU */
  return attr_write_raw(w, xStunAttrType_ChannelNumber, value, 4);
}

xErrno xStunAttrWriteData(xStunAttrWriter *w, const uint8_t *data, size_t len) {
  return attr_write_raw(w, xStunAttrType_Data, data, (uint16_t)len);
}

/* ───────────────────── Error Code ───────────────────── */

xErrno xStunAttrWriteErrorCode(xStunAttrWriter *w, int code,
                               const char *reason) {
  if (code < 300 || code > 699) return xErrno_InvalidArg;

  size_t   reason_len = reason ? strlen(reason) : 0;
  uint16_t value_len  = (uint16_t)(4 + reason_len);

  uint8_t value[256];
  if (value_len > sizeof(value)) return xErrno_NoMemory;

  value[0] = 0;
  value[1] = 0;
  value[2] = (uint8_t)(code / 100); /* class */
  value[3] = (uint8_t)(code % 100); /* number */
  if (reason_len > 0) {
    memcpy(value + 4, reason, reason_len);
  }

  return attr_write_raw(w, xStunAttrType_ErrorCode, value, value_len);
}

/* ───────────────────── Attribute Iterator ───────────────────── */

void xStunAttrIterInit(xStunAttrIter *iter, const xStunMsg *msg) {
  iter->data = msg->attrs;
  iter->len  = msg->attrs_len;
  iter->pos  = 0;
}

bool xStunAttrIterNext(xStunAttrIter *iter, xStunAttr *attr) {
  if (iter->pos + XSTUN_ATTR_HEADER_SIZE > iter->len) return false;

  const uint8_t *p = iter->data + iter->pos;
  attr->type       = (xStunAttrType)xReadU16BE(p);
  attr->length     = xReadU16BE(p + 2);

  if (iter->pos + XSTUN_ATTR_HEADER_SIZE + attr->length > iter->len) {
    return false;
  }

  attr->value = p + XSTUN_ATTR_HEADER_SIZE;
  iter->pos += XSTUN_ATTR_HEADER_SIZE + XSTUN_ALIGN4(attr->length);
  return true;
}

/* ───────────────────── Address Decoding ───────────────────── */

static xErrno decode_address(const xStunAttr *attr, bool xor_decode,
                             const uint8_t            txn_id[XSTUN_TXN_ID_SIZE],
                             struct sockaddr_storage *out) {
  if (!attr || !out) return xErrno_InvalidArg;
  if (attr->length < 4) return xErrno_InvalidArg;

  const uint8_t *v      = attr->value;
  uint8_t        family = v[1];

  memset(out, 0, sizeof(*out));

  if (family == 0x01) { /* IPv4 */
    if (attr->length < 8) return xErrno_InvalidArg;

    struct sockaddr_in *a4 = (struct sockaddr_in *)out;
    a4->sin_family         = AF_INET;

    uint16_t port = xReadU16BE(v + 2);
    uint32_t ip   = xReadU32BE(v + 4);

    if (xor_decode) {
      port ^= (uint16_t)(XSTUN_MAGIC_COOKIE >> 16);
      ip ^= XSTUN_MAGIC_COOKIE;
    }

    a4->sin_port        = htons(port);
    a4->sin_addr.s_addr = htonl(ip);
  } else if (family == 0x02) { /* IPv6 */
    if (attr->length < 20) return xErrno_InvalidArg;

    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)out;
    a6->sin6_family         = AF_INET6;

    uint16_t port = xReadU16BE(v + 2);
    if (xor_decode) {
      port ^= (uint16_t)(XSTUN_MAGIC_COOKIE >> 16);
    }
    a6->sin6_port = htons(port);

    memcpy(a6->sin6_addr.s6_addr, v + 4, 16);
    if (xor_decode && txn_id) {
      uint8_t xor_key[16];
      xWriteU32BE(xor_key, XSTUN_MAGIC_COOKIE);
      memcpy(xor_key + 4, txn_id, XSTUN_TXN_ID_SIZE);
      for (int i = 0; i < 16; i++) {
        a6->sin6_addr.s6_addr[i] ^= xor_key[i];
      }
    }
  } else {
    return xErrno_InvalidArg;
  }

  return xErrno_Ok;
}

xErrno xStunAttrDecodeXorMappedAddress(const xStunAttr *attr,
                                       const uint8_t txn_id[XSTUN_TXN_ID_SIZE],
                                       struct sockaddr_storage *out) {
  return decode_address(attr, true, txn_id, out);
}

xErrno xStunAttrDecodeMappedAddress(const xStunAttr         *attr,
                                    struct sockaddr_storage *out) {
  return decode_address(attr, false, NULL, out);
}

/* ───────────────────── Error Code Decoding ───────────────────── */

xErrno xStunAttrDecodeErrorCode(const xStunAttr *attr, int *code,
                                const char **reason, size_t *reason_len) {
  if (!attr || !code) return xErrno_InvalidArg;
  if (attr->length < 4) return xErrno_InvalidArg;

  int cls = attr->value[2] & 0x07;
  int num = attr->value[3];
  *code   = cls * 100 + num;

  if (reason && reason_len) {
    if (attr->length > 4) {
      *reason     = (const char *)(attr->value + 4);
      *reason_len = attr->length - 4;
    } else {
      *reason     = NULL;
      *reason_len = 0;
    }
  }

  return xErrno_Ok;
}

/* ───────────────────── Integrity / Fingerprint Verification
 * ───────────────────── */

xErrno xStunAttrVerifyMessageIntegrity(const uint8_t *msg_buf,
                                       size_t msg_len __attribute__((unused)),
                                       const xStunAttr *attr,
                                       const uint8_t *key, size_t key_len) {
  if (!msg_buf || !attr || !key) return xErrno_InvalidArg;
  if (attr->length != XSTUN_SHA1_DIGEST_SIZE) return xErrno_InvalidArg;

  /*
   * The MESSAGE-INTEGRITY is computed over the message up to (but not
   * including) the MESSAGE-INTEGRITY attribute itself, with the length
   * field adjusted to include the MESSAGE-INTEGRITY TLV.
   */
  size_t mi_offset = (size_t)(attr->value - XSTUN_ATTR_HEADER_SIZE - msg_buf);
  size_t hash_len  = mi_offset; /* bytes to hash = everything before MI attr */

  /* Create a temporary copy to adjust the length field */
  uint8_t header_copy[XSTUN_HEADER_SIZE];
  memcpy(header_copy, msg_buf, XSTUN_HEADER_SIZE);

  /* Adjust length to include MI attribute */
  uint16_t adjusted_len =
    (uint16_t)(mi_offset - XSTUN_HEADER_SIZE + XSTUN_ATTR_HEADER_SIZE +
               XSTUN_SHA1_DIGEST_SIZE);
  xWriteU16BE(header_copy + 2, adjusted_len);

  /*
   * We need to hash: adjusted_header + original_attrs_before_MI.
   * Since we can't modify the original buffer, we hash in two parts
   * using a temporary buffer.
   */
  size_t   total_hash_len = hash_len;
  uint8_t *tmp            = (uint8_t *)malloc(total_hash_len);
  if (!tmp) return xErrno_NoMemory;

  memcpy(tmp, header_copy, XSTUN_HEADER_SIZE);
  if (hash_len > XSTUN_HEADER_SIZE) {
    memcpy(tmp + XSTUN_HEADER_SIZE, msg_buf + XSTUN_HEADER_SIZE,
           hash_len - XSTUN_HEADER_SIZE);
  }

  uint8_t computed[XSTUN_SHA1_DIGEST_SIZE];
  xHmacSha1(key, key_len, tmp, total_hash_len, computed);
  free(tmp);

  if (memcmp(computed, attr->value, XSTUN_SHA1_DIGEST_SIZE) != 0) {
    return xErrno_InvalidArg;
  }

  return xErrno_Ok;
}

xErrno xStunAttrVerifyFingerprint(const uint8_t *msg_buf,
                                  size_t msg_len __attribute__((unused)),
                                  const xStunAttr *attr) {
  if (!msg_buf || !attr) return xErrno_InvalidArg;
  if (attr->length != 4) return xErrno_InvalidArg;

  /* FINGERPRINT offset */
  size_t fp_offset = (size_t)(attr->value - XSTUN_ATTR_HEADER_SIZE - msg_buf);

  /* Adjust length to include FINGERPRINT */
  uint8_t header_copy[XSTUN_HEADER_SIZE];
  memcpy(header_copy, msg_buf, XSTUN_HEADER_SIZE);
  uint16_t adjusted_len =
    (uint16_t)(fp_offset - XSTUN_HEADER_SIZE + XSTUN_ATTR_HEADER_SIZE + 4);
  xWriteU16BE(header_copy + 2, adjusted_len);

  /* CRC over adjusted header + attrs before FINGERPRINT */
  size_t   crc_len = fp_offset;
  uint8_t *tmp     = (uint8_t *)malloc(crc_len);
  if (!tmp) return xErrno_NoMemory;

  memcpy(tmp, header_copy, XSTUN_HEADER_SIZE);
  if (crc_len > XSTUN_HEADER_SIZE) {
    memcpy(tmp + XSTUN_HEADER_SIZE, msg_buf + XSTUN_HEADER_SIZE,
           crc_len - XSTUN_HEADER_SIZE);
  }

  uint32_t computed = xCrc32(tmp, crc_len) ^ XSTUN_FINGERPRINT_XOR;
  free(tmp);

  uint32_t expected = xReadU32BE(attr->value);
  if (computed != expected) {
    return xErrno_InvalidArg;
  }

  return xErrno_Ok;
}
