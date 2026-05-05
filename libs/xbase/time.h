/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * time.h - Time utilities
 */

#ifndef XBASE_TIME_H
#define XBASE_TIME_H

#include <stdint.h>
#include <xbase/base.h>

/**
 * @brief Return the current monotonic time in milliseconds (CLOCK_MONOTONIC).
 *
 * A clock that never goes backward and is unaffected by wall-clock adjustments.
 * Suitable for measuring elapsed time, timeouts, and intervals.
 *
 * @return Current monotonic time in milliseconds.
 */
XCAPI(uint64_t) xMonoMs(void);

/**
 * @brief Return the current wall-clock time in milliseconds (CLOCK_REALTIME).
 *
 * Named "Wall" after the common term "wall clock". Unlike xMonoMs(), this
 * clock may jump forward or backward due to NTP adjustments or manual changes.
 * Use this only when you need calendar/epoch time (e.g. timestamps in logs).
 *
 * @return Current wall-clock time in milliseconds since the Unix epoch.
 */
XCAPI(uint64_t) xWallMs(void);

#endif /* XBASE_TIME_H */
