/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * js_string.c - xjs string (UTF-16 storage, independent lifetime).
 */

#include "js.h"
#include "js_private.h"

#include <stdlib.h>
#include <string.h>

static xJSStringRef string_alloc(size_t len) {
  /* Single allocation: header + (len + 1) UTF-16 code units
   * (the extra slot is the NUL terminator).  calloc zero-fills
   * both the header and the buffer, so we still get a terminated
   * empty string for free. */
  struct OpaqueXJSString *s = (struct OpaqueXJSString *)calloc(
    1, sizeof(*s) + (len + 1) * sizeof(uint16_t));
  if (!s) return NULL;
  s->refcount = 1;
  s->length   = len;
  s->data     = (uint16_t *)(s + 1);
  return s;
}

xJSStringRef xJSStringCreateWithCharacters(const uint16_t *chars,
                                           size_t          numChars) {
  xJSStringRef s = string_alloc(numChars);
  if (!s) return NULL;
  if (chars && numChars) {
    memcpy(s->data, chars, numChars * sizeof(uint16_t));
  }
  return s;
}

xJSStringRef xJSStringCreateWithUTF8CString(const char *cstr) {
  if (!cstr) return string_alloc(0);
  size_t       src_len = strlen(cstr);
  size_t       ulen    = xjs_utf8_to_utf16(cstr, src_len, NULL, 0);
  xJSStringRef s       = string_alloc(ulen);
  if (!s) return NULL;
  xjs_utf8_to_utf16(cstr, src_len, s->data, ulen);
  return s;
}

xJSStringRef xJSStringRetain(xJSStringRef s) {
  if (s) s->refcount++;
  return s;
}

void xJSStringRelease(xJSStringRef s) {
  if (!s) return;
  if (--s->refcount > 0) return;
  free(s);
}

size_t xJSStringGetLength(xJSStringRef s) {
  return s ? s->length : 0;
}

const uint16_t *xJSStringGetCharactersPtr(xJSStringRef s) {
  return s ? s->data : NULL;
}

size_t xJSStringGetMaximumUTF8CStringSize(xJSStringRef s) {
  /* worst case: every code unit expands to 3 bytes + NUL */
  return s ? s->length * 3 + 1 : 1;
}

size_t xJSStringGetUTF8CString(xJSStringRef s, char *buffer,
                               size_t bufferSize) {
  if (!buffer || bufferSize == 0) return 0;
  if (!s) {
    buffer[0] = 0;
    return 0;
  }
  size_t n = xjs_utf16_to_utf8(s->data, s->length, buffer, bufferSize - 1);
  if (n >= bufferSize) n = bufferSize - 1;
  buffer[n] = 0;
  return n + 1; /* JSC returns the size including the trailing NUL */
}

bool xJSStringIsEqual(xJSStringRef a, xJSStringRef b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->length != b->length) return false;
  return memcmp(a->data, b->data, a->length * sizeof(uint16_t)) == 0;
}

bool xJSStringIsEqualToUTF8CString(xJSStringRef a, const char *b) {
  if (!a || !b) return false;
  xJSStringRef tmp = xJSStringCreateWithUTF8CString(b);
  bool         eq  = xJSStringIsEqual(a, tmp);
  xJSStringRelease(tmp);
  return eq;
}
