/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef IC_STR_H
#define IC_STR_H

//-------------------------------------------------------------
// ssize_t-flavored string and memory byte helpers used across
// xline internals. Public xLineStartsWith / xLineIstartsWith
// live in line.h; those are implemented in str.c as well.
//-------------------------------------------------------------

#include "platform.h"

// --- Memory primitives (bounded / asserting wrappers) ---------

ic_private void ic_memmove(void *dest, const void *src, ssize_t n);
ic_private void ic_memcpy(void *dest, const void *src, ssize_t n);
ic_private void ic_memset(void *dest, uint8_t value, ssize_t n);
ic_private bool ic_memnmove(void *dest, ssize_t dest_size, const void *src, ssize_t n);

// --- String primitives ----------------------------------------

ic_private ssize_t ic_strlen(const char *s);
ic_private bool    ic_strcpy(char *dest, ssize_t dest_size /* including 0 */, const char *src);
ic_private bool    ic_strncpy(char *dest, ssize_t dest_size /* including 0 */, const char *src,
                              ssize_t n);

// NULL-safe heap duplicators. Return a freshly malloc'd copy (caller
// frees with free()), or NULL if the input is NULL / n < 0 / OOM.
ic_private char *ic_strdup(const char *s);
ic_private char *ic_strndup(const char *s, ssize_t n);

ic_private bool ic_contains(const char *big, const char *s);
ic_private bool ic_icontains(const char *big, const char *s);
ic_private char ic_tolower(char c);
ic_private void ic_str_tolower(char *s);
ic_private int  ic_stricmp(const char *s1, const char *s2);
ic_private int  ic_strnicmp(const char *s1, const char *s2, ssize_t n);

#endif // IC_STR_H
