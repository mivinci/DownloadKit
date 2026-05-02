/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_PLATFORM_H
#define IC_PLATFORM_H

//-------------------------------------------------------------
// Platform glue: compiler/visibility macros, ssize_t adaptor,
// and the forward declaration of the top-level environment
// handle that threads through the whole codebase.
//-------------------------------------------------------------

#include <sys/types.h>  // ssize_t (posix)
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

// --- extern "C" / visibility ----------------------------------

# ifdef __cplusplus
#  define ic_extern_c   extern "C"
# else
#  define ic_extern_c
# endif

#if defined(IC_SEPARATE_OBJS)
#  define ic_public     ic_extern_c
# if defined(__GNUC__) // includes clang and icc
#  define ic_private    __attribute__((visibility("hidden")))
# else
#  define ic_private
# endif
#else
# define ic_private     static
# define ic_public      ic_extern_c
#endif

#define ic_unused(x)    (void)(x)

// Attribute marker for file-local helpers we intentionally keep even
// when nobody currently calls them (upstream's alternate code paths /
// future key bindings). Keeps -Wunused-function silent without
// resorting to a module-wide -Wno-unused-function override.
#if defined(__GNUC__) || defined(__clang__)
#  define ic_unused_fn  __attribute__((unused))
#else
#  define ic_unused_fn
#endif

// --- ssize_t helpers ------------------------------------------

#if defined(_MSC_VER)
typedef intptr_t ssize_t;
#endif

#define ssizeof(tp)   (ssize_t)(sizeof(tp))
static inline size_t  to_size_t(ssize_t sz) { return (sz >= 0 ? (size_t)sz : 0); }
static inline ssize_t to_ssize_t(size_t sz) { return (sz <= SIZE_MAX/2 ? (ssize_t)sz : 0); }

// --- Environment forward decl ---------------------------------

struct ic_env_s;
typedef struct ic_env_s ic_env_t;

#endif // IC_PLATFORM_H
