/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <string.h>

#include "str.h"

//-------------------------------------------------------------
// ssize_t-wrapped memory helpers
//-------------------------------------------------------------

ic_private ssize_t ic_strlen(const char *s) {
  if (s == NULL) return 0;
  return to_ssize_t(strlen(s));
}

ic_private void ic_memmove(void *dest, const void *src, ssize_t n) {
  assert(dest != NULL && src != NULL);
  if (n <= 0) return;
  memmove(dest, src, to_size_t(n));
}

ic_private void ic_memcpy(void *dest, const void *src, ssize_t n) {
  assert(dest != NULL && src != NULL);
  if (dest == NULL || src == NULL || n <= 0) return;
  memcpy(dest, src, to_size_t(n));
}

ic_private void ic_memset(void *dest, uint8_t value, ssize_t n) {
  assert(dest != NULL);
  if (dest == NULL || n <= 0) return;
  memset(dest, (int8_t)value, to_size_t(n));
}

ic_private bool ic_memnmove(void *dest, ssize_t dest_size, const void *src,
                            ssize_t n) {
  assert(dest != NULL && src != NULL);
  if (n <= 0) return true;
  if (dest_size < n) {
    assert(false);
    return false;
  }
  memmove(dest, src, to_size_t(n));
  return true;
}

ic_private bool ic_strcpy(char *dest, ssize_t dest_size /* including 0 */,
                          const char *src) {
  assert(dest != NULL && src != NULL);
  if (dest == NULL || dest_size <= 0) return false;
  ssize_t slen = ic_strlen(src);
  if (slen >= dest_size) return false;
  strcpy(dest, src);
  assert(dest[slen] == 0);
  return true;
}

ic_private bool ic_strncpy(char *dest, ssize_t dest_size /* including 0 */,
                           const char *src, ssize_t n) {
  assert(dest != NULL && n < dest_size);
  if (dest == NULL || dest_size <= 0) return false;
  if (n >= dest_size) return false;
  if (src == NULL || n <= 0) {
    dest[0] = 0;
  } else {
    strncpy(dest, src, to_size_t(n));
    dest[n] = 0;
  }
  return true;
}

//-------------------------------------------------------------
// String matching
//-------------------------------------------------------------

ic_public bool xLineStartsWith(const char *s, const char *prefix) {
  if (s == prefix) return true;
  if (prefix == NULL) return true;
  if (s == NULL) return false;

  ssize_t i;
  for (i = 0; s[i] != 0 && prefix[i] != 0; i++) {
    if (s[i] != prefix[i]) return false;
  }
  return (prefix[i] == 0);
}

ic_private char ic_tolower(char c) {
  return (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}

ic_private void ic_str_tolower(char *s) {
  while (*s != 0) {
    *s = ic_tolower(*s);
    s++;
  }
}

ic_public bool xLineIstartsWith(const char *s, const char *prefix) {
  if (s == prefix) return true;
  if (prefix == NULL) return true;
  if (s == NULL) return false;

  ssize_t i;
  for (i = 0; s[i] != 0 && prefix[i] != 0; i++) {
    if (ic_tolower(s[i]) != ic_tolower(prefix[i])) return false;
  }
  return (prefix[i] == 0);
}

ic_private int ic_strnicmp(const char *s1, const char *s2, ssize_t n) {
  if (s1 == NULL && s2 == NULL) return 0;
  if (s1 == NULL) return -1;
  if (s2 == NULL) return 1;
  ssize_t i;
  for (i = 0; s1[i] != 0 && i < n;
       i++) { // note: if s2[i] == 0 the loop will stop as c1 != c2
    char c1 = ic_tolower(s1[i]);
    char c2 = ic_tolower(s2[i]);
    if (c1 < c2) return -1;
    if (c1 > c2) return 1;
  }
  return ((i >= n || s2[i] == 0) ? 0 : -1);
}

ic_private int ic_stricmp(const char *s1, const char *s2) {
  ssize_t len1 = ic_strlen(s1);
  ssize_t len2 = ic_strlen(s2);
  if (len1 < len2) return -1;
  if (len1 > len2) return 1;
  return (ic_strnicmp(s1, s2, (len1 >= len2 ? len1 : len2)));
}

static const char *ic_stristr(const char *s, const char *pat) {
  if (s == NULL) return NULL;
  if (pat == NULL || pat[0] == 0) return s;
  ssize_t patlen = ic_strlen(pat);
  for (ssize_t i = 0; s[i] != 0; i++) {
    if (ic_strnicmp(s + i, pat, patlen) == 0) return (s + i);
  }
  return NULL;
}

ic_private bool ic_contains(const char *big, const char *s) {
  if (big == NULL) return false;
  if (s == NULL) return true;
  return (strstr(big, s) != NULL);
}

ic_private bool ic_icontains(const char *big, const char *s) {
  if (big == NULL) return false;
  if (s == NULL) return true;
  return (ic_stristr(big, s) != NULL);
}
