/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_DEBUG_H
#define IC_DEBUG_H

//-------------------------------------------------------------
// Diagnostic logging.
//
// Enabled at runtime via ISOCLINE_DEBUG env var. Compile with
// -DIC_NO_DEBUG_MSG to strip out completely, or with
// -DIC_DEBUG_TO_FILE to redirect into isocline.debug.txt instead
// of stderr.
//-------------------------------------------------------------

#include "platform.h"

#if defined(IC_NO_DEBUG_MSG)
#define debug_msg(fmt,...)   (void)(0)
#else
ic_private void debug_msg(const char* fmt, ...);
#endif

#endif // IC_DEBUG_H
